#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#define native_fileno _fileno
#else
#include <unistd.h>
#define native_fileno fileno
#endif
#include "esp_err.h"
#include "esp_check.h"
#include "storage_copy_types.h"

static unsigned checks;
#define CHECK(value) do { ++checks; if (!(value)) { \
    fprintf(stderr,"Storage copy check at %u: %s\n",__LINE__,#value); exit(1); } } while (0)
#define TAG "storage-copy-check"
#define CERBERUS_STORAGE_MOUNT_PATH test_root
typedef int *SemaphoreHandle_t;
static SemaphoreHandle_t storage_mutex;
static int mutex_value;
static const char *test_root;
static const char *test_recon_root; /* Inactive here; active lease has its own fault suite. */
static bool sd_mounted=true, locked, fail_mutex_create, fail_mutex_take;
static uint64_t sd_ward_log_lease;
static void *sd_gps;
static cerberus_storage_owner_t storage_owner=CERBERUS_STORAGE_OWNER_FIRMWARE;
static unsigned lock_attempts, releases, open_streams, yields;
static FILE *source_stream,*temp_stream;
static char source_file[1024],destination_file[1024],temp_file[1040];
static uint8_t payload[SD_HASH_BUFFER_SIZE*3+37];
static size_t payload_size,short_read_limit;
static bool injected_source_error, zero_without_eof, read_error_with_progress;
static bool source_close_failed,temp_close_failed,temp_flushed,temp_synced;
enum event { STAT_SOURCE,STAT_DESTINATION,STAT_TEMP,SOURCE_OPEN,TEMP_OPEN,READ,
    WRITE,FLUSH,SYNC,SOURCE_CLOSE,TEMP_CLOSE,RENAME,REMOVE,EVENT_COUNT };
static unsigned counts[EVENT_COUNT],failure_at[EVENT_COUNT];
static unsigned sequence[4096],sequence_count;

static bool event(enum event kind)
{
    CHECK(locked && sd_mounted && storage_owner==CERBERUS_STORAGE_OWNER_FIRMWARE);
    CHECK(sequence_count<sizeof(sequence)/sizeof(sequence[0]));
    sequence[sequence_count++]=(unsigned)kind;
    ++counts[kind];
    return failure_at[kind] && counts[kind]==failure_at[kind];
}
static SemaphoreHandle_t xSemaphoreCreateMutex(void)
{return fail_mutex_create?NULL:&mutex_value;}
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
static int xSemaphoreTake(SemaphoreHandle_t mutex,unsigned timeout)
{
    CHECK(mutex==&mutex_value && timeout==1000 && !locked);++lock_attempts;
    if(fail_mutex_take)return 0;
    locked=true;return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex)
{CHECK(mutex==&mutex_value && locked && !open_streams);locked=false;++releases;}
static void vTaskDelay(unsigned ticks)
{CHECK(locked && ticks==1);++yields;}
static bool path_equal(const char *left,const char *right)
{
#ifdef _WIN32
    return _stricmp(left,right)==0;
#else
    return strcmp(left,right)==0;
#endif
}

static int injected_stat(const char *path,struct stat *result)
{
    enum event kind=path_equal(path,source_file)?STAT_SOURCE:
        path_equal(path,destination_file)?STAT_DESTINATION:STAT_TEMP;
    CHECK(kind!=STAT_TEMP || path_equal(path,temp_file));
    if(event(kind)){errno=EIO;return -1;}
    return stat(path,result);
}
static FILE *injected_fopen(const char *path,const char *mode)
{
    bool source=!strcmp(mode,"rb");
    CHECK(!strcmp(path,source?source_file:temp_file));
    CHECK(source || (!strcmp(mode,"wb") && source_stream && !temp_stream));
    if(event(source?SOURCE_OPEN:TEMP_OPEN)){errno=EIO;return NULL;}
    FILE *stream=fopen(path,mode);
    CHECK(stream!=NULL);++open_streams;
    if(source)source_stream=stream;else temp_stream=stream;
    return stream;
}
static size_t injected_fread(void *data,size_t size,size_t count,FILE *stream)
{
    CHECK(stream==source_stream && size==1 && count==sizeof(sd_hash_buffer));
    if(event(READ)){
        if(zero_without_eof)return 0;
        injected_source_error=true;
        return read_error_with_progress?fread(data,size,17,stream):0;
    }
    if(short_read_limit && count>short_read_limit)count=short_read_limit;
    return fread(data,size,count,stream);
}
static size_t injected_fwrite(const void *data,size_t size,size_t count,FILE *stream)
{
    CHECK(stream==temp_stream && size==1 && count>0);
    if(event(WRITE))return count>1?fwrite(data,size,count/2,stream):0;
    return fwrite(data,size,count,stream);
}
static int injected_ferror(FILE *stream)
{CHECK(stream==source_stream);return injected_source_error||ferror(stream);}
static int injected_feof(FILE *stream)
{CHECK(stream==source_stream);return feof(stream);}
static int injected_fflush(FILE *stream)
{
    CHECK(stream==temp_stream);
    if(event(FLUSH)){errno=EIO;return EOF;}
    int result=fflush(stream);temp_flushed=result==0;return result;
}
static int injected_fsync(int descriptor)
{
    CHECK(temp_stream && temp_flushed && descriptor==native_fileno(temp_stream));
    if(event(SYNC)){errno=EIO;return -1;}
    /* The syscall result is injected; host tests do not prove SD durability. */
    temp_synced=true;return 0;
}
static int injected_fclose(FILE *stream)
{
    bool source=stream==source_stream;
    CHECK(source || stream==temp_stream);
    bool fail=event(source?SOURCE_CLOSE:TEMP_CLOSE);
    int result=fclose(stream);CHECK(result==0 && open_streams>0);--open_streams;
    if(source){source_stream=NULL;source_close_failed=fail;}
    else {temp_stream=NULL;temp_close_failed=fail;}
    /* fclose consumes its stream even when its final flush reports failure. */
    if(fail){errno=EIO;return EOF;}return result;
}
static int injected_rename(const char *from,const char *to)
{
    CHECK(!strcmp(from,temp_file) && !strcmp(to,destination_file));
    CHECK(!open_streams && !source_stream && !temp_stream);
    CHECK(temp_flushed && temp_synced && !source_close_failed && !temp_close_failed);
    if(event(RENAME)){errno=EIO;return -1;}
    return rename(from,to);
}
static int injected_remove(const char *path)
{
    CHECK(!strcmp(path,temp_file) && !open_streams);
    if(event(REMOVE)){errno=EIO;return -1;}
    return remove(path);
}
#define stat(path, result) injected_stat(path,result)
#define fopen(path, mode) injected_fopen(path,mode)
#define fread(data, size, count, stream) injected_fread(data,size,count,stream)
#define fwrite(data, size, count, stream) injected_fwrite(data,size,count,stream)
#define ferror(stream) injected_ferror(stream)
#define feof(stream) injected_feof(stream)
#define fflush(stream) injected_fflush(stream)
#define fsync(descriptor) injected_fsync(descriptor)
#define fileno(stream) native_fileno(stream)
#define fclose(stream) injected_fclose(stream)
#define rename(from, to) injected_rename(from,to)
#define remove(path) injected_remove(path)
#include "storage_copy_impl.h"
#undef stat
#undef fopen
#undef fread
#undef fwrite
#undef ferror
#undef feof
#undef fflush
#undef fsync
#undef fileno
#undef fclose
#undef rename
#undef remove

/* Compile the actual Paste result propagation. Rendering/list refresh remain
 * boundary fakes; the copy branch calls the actual production copy above. */
static bool file_clipboard_active,file_clipboard_move,file_clipboard_is_directory;
static char file_clipboard_path[CERBERUS_STORAGE_PATH_MAX],file_path[CERBERUS_STORAGE_PATH_MAX];
static char file_clipboard_name[CERBERUS_STORAGE_NAME_MAX],ui_message[64];
static unsigned file_page_offset,ui_refreshes,ui_draws;
static bool ui_success;
static const char *esp_err_to_name(esp_err_t error)
{return error==ESP_FAIL?"ESP_FAIL":error==ESP_ERR_NOT_SUPPORTED?"ESP_ERR_NOT_SUPPORTED":"ESP_ERR_INVALID_STATE";}
static void file_set_action_status(const char *message,bool success)
{CHECK(!locked);snprintf(ui_message,sizeof(ui_message),"%s",message);ui_success=success;}
static esp_err_t refresh_file_listing(bool clear)
{CHECK(!clear && !locked && !open_streams);++ui_refreshes;return ESP_OK;}
static esp_err_t draw_detail_page(void)
{CHECK(!locked && !open_streams);++ui_draws;return ESP_OK;}
static esp_err_t cerberus_storage_move_entry(const char *a,const char *b,const char *c,const char *d)
{(void)a;(void)b;(void)c;(void)d;CHECK(false);return ESP_FAIL;}
#define ESP_ERROR_CHECK(call) CHECK((call)==ESP_OK)
#include "storage_copy_ui_impl.h"

static bool exists(const char *path)
{struct stat item;return stat(path,&item)==0;}
static void write_fixture(const char *path,const void *data,size_t length)
{
    FILE *stream=fopen(path,"wb");CHECK(stream!=NULL);
    CHECK(fwrite(data,1,length,stream)==length && fclose(stream)==0);
}
static void exact_file(const char *path,const uint8_t *data,size_t length)
{
    FILE *stream=fopen(path,"rb");CHECK(stream!=NULL);
    for(size_t offset=0;offset<length;){
        uint8_t bytes[521];size_t amount=length-offset;if(amount>sizeof(bytes))amount=sizeof(bytes);
        CHECK(fread(bytes,1,amount,stream)==amount && !memcmp(bytes,data+offset,amount));offset+=amount;
    }
    CHECK(fgetc(stream)==EOF && !ferror(stream));CHECK(fclose(stream)==0);
}
static void seed(size_t length)
{
    CHECK(!locked && !open_streams);
    (void)remove(source_file);(void)remove(destination_file);(void)remove(temp_file);
    memset(counts,0,sizeof(counts));memset(failure_at,0,sizeof(failure_at));
    memset(sequence,0,sizeof(sequence));sequence_count=0;
    lock_attempts=releases=yields=0;storage_mutex=NULL;
    fail_mutex_create=fail_mutex_take=false;sd_mounted=true;storage_owner=CERBERUS_STORAGE_OWNER_FIRMWARE;
    sd_ward_log_lease=0;
    sd_gps=NULL;
    injected_source_error=zero_without_eof=read_error_with_progress=false;
    source_close_failed=temp_close_failed=temp_flushed=temp_synced=false;
    source_stream=temp_stream=NULL;short_read_limit=0;payload_size=length;
    CHECK(length<=sizeof(payload));write_fixture(source_file,payload,length);
}
static esp_err_t copy(void)
{return cerberus_storage_copy_file("/src","SOURCE.BIN","/dst","COPY.BIN");}
static void released(void)
{
    CHECK(!locked && !open_streams && !source_stream && !temp_stream);
    CHECK(storage_owner==CERBERUS_STORAGE_OWNER_FIRMWARE && sd_mounted);
    CHECK(lock_attempts==1 && releases==1);
    CHECK(counts[SOURCE_CLOSE]<=1 && counts[TEMP_CLOSE]<=1);
    exact_file(source_file,payload,payload_size);
}
static void successes(void)
{
    const size_t lengths[]={0,1,37,SD_HASH_BUFFER_SIZE-1,SD_HASH_BUFFER_SIZE,
        SD_HASH_BUFFER_SIZE+1,SD_HASH_BUFFER_SIZE*3,sizeof(payload)};
    for(unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);++i){
        seed(lengths[i]);CHECK(copy()==ESP_OK);released();
        exact_file(destination_file,payload,payload_size);CHECK(!exists(temp_file));
        CHECK(counts[SOURCE_OPEN]==1 && counts[TEMP_OPEN]==1 && counts[SOURCE_CLOSE]==1 && counts[TEMP_CLOSE]==1);
        CHECK(counts[FLUSH]==1 && counts[SYNC]==1 && counts[RENAME]==1 && !counts[REMOVE]);
        CHECK(yields==lengths[i]/SD_HASH_BUFFER_SIZE);
    }
    const size_t short_limits[]={1,17,257,SD_HASH_BUFFER_SIZE-1};
    for(unsigned i=0;i<sizeof(short_limits)/sizeof(short_limits[0]);++i){
        seed(i==0?37:sizeof(payload));short_read_limit=short_limits[i];
        CHECK(copy()==ESP_OK);released();exact_file(destination_file,payload,payload_size);
        CHECK(counts[READ]>1 && yields>0 && !exists(temp_file) && counts[RENAME]==1);
    }
}
static void operation_failures(void)
{
    const enum event failures[]={SOURCE_OPEN,TEMP_OPEN,READ,WRITE,FLUSH,SYNC,SOURCE_CLOSE,TEMP_CLOSE,RENAME};
    for(unsigned i=0;i<sizeof(failures)/sizeof(failures[0]);++i){
        seed(sizeof(payload));enum event stage=failures[i];failure_at[stage]=1;
        CHECK(copy()==ESP_FAIL);released();CHECK(!exists(destination_file) && !exists(temp_file));
        CHECK(counts[stage]==1);
        CHECK(counts[TEMP_OPEN]==(stage!=SOURCE_OPEN));
        CHECK(counts[SOURCE_CLOSE]==(stage!=SOURCE_OPEN));
        CHECK(counts[TEMP_CLOSE]==(stage!=SOURCE_OPEN && stage!=TEMP_OPEN));
        CHECK(counts[REMOVE]==(stage!=SOURCE_OPEN && stage!=TEMP_OPEN));
        CHECK(counts[RENAME]==(stage==RENAME));
        if(stage==FLUSH)CHECK(!counts[SYNC]);
    }
    for(unsigned stage=READ;stage<=WRITE;++stage)for(unsigned nth=2;nth<=3;++nth){
        seed(sizeof(payload));failure_at[stage]=nth;
        CHECK(copy()==ESP_FAIL);released();CHECK(counts[stage]==nth && !counts[RENAME]);
        CHECK(counts[REMOVE]==1 && !exists(temp_file) && !exists(destination_file));
    }
    seed(sizeof(payload));failure_at[READ]=2;read_error_with_progress=true;
    CHECK(copy()==ESP_FAIL);released();CHECK(counts[READ]==2 && !counts[RENAME] && !exists(destination_file));
    seed(sizeof(payload));failure_at[READ]=1;zero_without_eof=true;
    CHECK(copy()==ESP_FAIL);released();CHECK(counts[READ]==1 && !yields && !counts[RENAME]);
    seed(37);short_read_limit=17;failure_at[READ]=2;zero_without_eof=true;
    CHECK(copy()==ESP_FAIL);released();CHECK(counts[READ]==2 && counts[WRITE]==1 && !counts[RENAME]);
    seed(0);failure_at[READ]=1;read_error_with_progress=true;
    CHECK(copy()==ESP_FAIL);released();CHECK(!counts[FLUSH] && !counts[RENAME] && !exists(destination_file));
    for(unsigned prior=0;prior<2;++prior){
        seed(sizeof(payload));failure_at[SOURCE_CLOSE]=failure_at[TEMP_CLOSE]=1;
        if(prior)failure_at[WRITE]=2;
        CHECK(copy()==ESP_FAIL);released();CHECK(counts[SOURCE_CLOSE]==1 && counts[TEMP_CLOSE]==1);
        CHECK(!counts[RENAME] && counts[REMOVE]==1 && !exists(temp_file) && !exists(destination_file));
    }
    /* Cleanup failure stays an error and leaves only this operation's temp.
     * A subsequent copy must treat the remnant as a collision, not overwrite it. */
    seed(sizeof(payload));failure_at[WRITE]=2;failure_at[REMOVE]=1;
    CHECK(copy()==ESP_FAIL);released();CHECK(counts[REMOVE]==1 && exists(temp_file) && !exists(destination_file));
    uint8_t remnant[sizeof(payload)];FILE *stream=fopen(temp_file,"rb");CHECK(stream!=NULL);
    size_t retained=fread(remnant,1,sizeof(remnant),stream);CHECK(retained>0 && fclose(stream)==0);
    memset(failure_at,0,sizeof(failure_at));unsigned opens=counts[TEMP_OPEN],removes=counts[REMOVE];
    CHECK(copy()==ESP_ERR_INVALID_STATE && !locked && !open_streams);
    CHECK(counts[TEMP_OPEN]==opens && counts[REMOVE]==removes);exact_file(temp_file,remnant,retained);
}
static void collisions_and_stat_errors(void)
{
    static const uint8_t sentinel[]={0xBE,0xEF,0,0x17,0x39};
    for(unsigned kind=0;kind<2;++kind){
        seed(sizeof(payload));const char *path=kind?temp_file:destination_file;
        write_fixture(path,sentinel,sizeof(sentinel));
        CHECK(copy()==ESP_ERR_INVALID_STATE);released();exact_file(path,sentinel,sizeof(sentinel));
        CHECK(!counts[SOURCE_OPEN] && !counts[TEMP_OPEN] && !counts[RENAME] && !counts[REMOVE]);
    }
    for(unsigned which=STAT_SOURCE;which<=STAT_TEMP;++which){
        seed(sizeof(payload));failure_at[which]=1;
        if(which==STAT_DESTINATION)write_fixture(destination_file,sentinel,sizeof(sentinel));
        if(which==STAT_TEMP)write_fixture(temp_file,sentinel,sizeof(sentinel));
        CHECK(copy()==(which==STAT_SOURCE?ESP_ERR_NOT_FOUND:ESP_FAIL));released();
        CHECK(!counts[SOURCE_OPEN] && !counts[TEMP_OPEN] && !counts[REMOVE]);
        if(which==STAT_DESTINATION)exact_file(destination_file,sentinel,sizeof(sentinel));
        if(which==STAT_TEMP)exact_file(temp_file,sentinel,sizeof(sentinel));
    }
    seed(37);CHECK(cerberus_storage_copy_file("/src","SOURCE.BIN","/src","SOURCE.BIN")==ESP_ERR_INVALID_STATE);
    released();CHECK(!counts[SOURCE_OPEN] && !counts[REMOVE]);
#ifdef _WIN32
    seed(37);CHECK(cerberus_storage_copy_file("/src","SOURCE.BIN","/src","source.bin")==ESP_ERR_INVALID_STATE);
    released();CHECK(!counts[SOURCE_OPEN] && !counts[REMOVE]);
#endif
    seed(37);char original_source[sizeof(source_file)];strcpy(original_source,source_file);
    write_fixture(temp_file,payload,payload_size);strcpy(source_file,temp_file);
    CHECK(cerberus_storage_copy_file("/dst","COPY.BIN.CERBTMP","/dst","COPY.BIN")==ESP_ERR_INVALID_STATE);
    released();CHECK(!counts[SOURCE_OPEN] && !counts[TEMP_OPEN] && !counts[REMOVE]);
    exact_file(original_source,payload,payload_size);strcpy(source_file,original_source);
}
static void admission(void)
{
    seed(37);sd_ward_log_lease=1;
    CHECK(copy()==ESP_ERR_INVALID_STATE);released();
    CHECK(!sequence_count && !exists(destination_file) && !exists(temp_file));
    sd_ward_log_lease=0;
    seed(37);sd_gps=(void *)1;
    CHECK(copy()==ESP_ERR_INVALID_STATE);released();
    CHECK(!sequence_count&&!exists(destination_file)&&!exists(temp_file));sd_gps=NULL;
    for(unsigned state=0;state<3;++state){
        seed(37);sd_mounted=state!=0;storage_owner=state==1?CERBERUS_STORAGE_OWNER_USB:CERBERUS_STORAGE_OWNER_NONE;
        CHECK(copy()==ESP_ERR_INVALID_STATE && !locked && releases==1 && !sequence_count);
        CHECK(storage_owner==(state==1?CERBERUS_STORAGE_OWNER_USB:CERBERUS_STORAGE_OWNER_NONE));
        exact_file(source_file,payload,payload_size);CHECK(!exists(destination_file) && !exists(temp_file));
    }
    seed(37);fail_mutex_create=true;
    CHECK(copy()==ESP_ERR_NO_MEM && !locked && !lock_attempts && !releases && !sequence_count);
    seed(37);fail_mutex_take=true;
    CHECK(copy()==ESP_ERR_TIMEOUT && !locked && lock_attempts==1 && !releases && !sequence_count);
    const char *bad_paths[]={NULL,"","src","/../src","/src\\x","/C:x"};
    for(unsigned i=0;i<sizeof(bad_paths)/sizeof(bad_paths[0]);++i){
        seed(37);CHECK(cerberus_storage_copy_file(bad_paths[i],"SOURCE.BIN","/dst","COPY.BIN")==ESP_ERR_INVALID_ARG);
        CHECK(!lock_attempts && !releases && !sequence_count);
        CHECK(cerberus_storage_copy_file("/src","SOURCE.BIN",bad_paths[i],"COPY.BIN")==ESP_ERR_INVALID_ARG);
    }
    const char *bad_names[]={NULL,"",".","..","a/b","a\\b","C:x"};
    for(unsigned i=0;i<sizeof(bad_names)/sizeof(bad_names[0]);++i){
        seed(37);CHECK(cerberus_storage_copy_file("/src",bad_names[i],"/dst","COPY.BIN")==ESP_ERR_INVALID_ARG);
        CHECK(cerberus_storage_copy_file("/src","SOURCE.BIN","/dst",bad_names[i])==ESP_ERR_INVALID_ARG);
        CHECK(!lock_attempts && !sequence_count);
    }
    seed(37);char long_name[CERBERUS_STORAGE_NAME_MAX+1];memset(long_name,'A',sizeof(long_name)-1);long_name[sizeof(long_name)-1]=0;
    CHECK(cerberus_storage_copy_file("/src","SOURCE.BIN","/dst",long_name)==ESP_ERR_INVALID_ARG);
    CHECK(!lock_attempts && !sequence_count);
}
static void paste_ui_contract(void)
{
    char original_destination[sizeof(destination_file)],original_temp[sizeof(temp_file)];
    strcpy(original_destination,destination_file);strcpy(original_temp,temp_file);
    CHECK(snprintf(destination_file,sizeof(destination_file),"%s/dst/SOURCE.BIN",test_root)>0);
    CHECK(snprintf(temp_file,sizeof(temp_file),"%s.CERBTMP",destination_file)>0);
    const enum event stages[]={EVENT_COUNT,SOURCE_OPEN,TEMP_OPEN,READ,WRITE,FLUSH,SYNC,SOURCE_CLOSE,TEMP_CLOSE,RENAME,REMOVE};
    for(unsigned i=0;i<sizeof(stages)/sizeof(stages[0]);++i){
        seed(sizeof(payload));file_clipboard_active=true;file_clipboard_move=false;file_clipboard_is_directory=false;
        strcpy(file_clipboard_path,"/src");strcpy(file_clipboard_name,"SOURCE.BIN");strcpy(file_path,"/dst");
        file_page_offset=12;ui_refreshes=ui_draws=0;ui_message[0]=0;ui_success=false;
        if(stages[i]!=EVENT_COUNT)failure_at[stages[i]]=1;
        if(stages[i]==REMOVE)failure_at[WRITE]=2;
        paste_file_clipboard();released();CHECK(ui_draws==1);
        CHECK(!strcmp(file_clipboard_path,"/src") && !strcmp(file_clipboard_name,"SOURCE.BIN") && !strcmp(file_path,"/dst"));
        if(stages[i]==EVENT_COUNT){
            CHECK(!file_clipboard_active && ui_success && !strcmp(ui_message,"COPY COMPLETE"));
            CHECK(!file_page_offset && ui_refreshes==1);exact_file(destination_file,payload,payload_size);
        }else{
            CHECK(file_clipboard_active && !ui_success && strstr(ui_message,"PASTE FAILED: ")==ui_message);
            CHECK(file_page_offset==12 && !ui_refreshes && !exists(destination_file));
        }
    }
    seed(37);file_clipboard_active=true;file_clipboard_is_directory=true;ui_refreshes=ui_draws=0;
    paste_file_clipboard();CHECK(file_clipboard_active && !ui_success && !ui_refreshes && ui_draws==1);
    CHECK(strstr(ui_message,"ESP_ERR_NOT_SUPPORTED") && !lock_attempts && !sequence_count);
    strcpy(destination_file,original_destination);strcpy(temp_file,original_temp);
}
int main(int argc,char **argv)
{
    CHECK(argc==2);test_root=argv[1];
    CHECK(snprintf(source_file,sizeof(source_file),"%s/src/SOURCE.BIN",test_root)>0);
    CHECK(snprintf(destination_file,sizeof(destination_file),"%s/dst/COPY.BIN",test_root)>0);
    CHECK(snprintf(temp_file,sizeof(temp_file),"%s.CERBTMP",destination_file)>0);
    for(size_t i=0;i<sizeof(payload);++i)payload[i]=(uint8_t)(i*37U+(i>>8U));
    successes();operation_failures();collisions_and_stat_errors();admission();paste_ui_contract();
    CHECK(!locked && !open_streams);
    printf("Storage file copy: %u checks; actual copy/path/owner/mutex/Paste code, real contents, short/EOF reads, per-stage faults, close gating, collision-safe cleanup and truthful UI result passed\n",checks);
    return 0;
}
