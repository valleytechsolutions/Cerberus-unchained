#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define native_open _open
#define native_fdopen _fdopen
#define native_close _close
#define native_fileno _fileno
#define native_chdir _chdir
#define native_mkdir(path) _mkdir(path)
#else
#include <unistd.h>
#define native_open open
#define native_fdopen fdopen
#define native_close close
#define native_fileno fileno
#define native_chdir chdir
#define native_mkdir(path) mkdir(path,0775)
#ifndef O_BINARY
#define O_BINARY 0
#endif
#endif
#include "esp_err.h"
#include "esp_check.h"
#include "gps_gpx.h"
#define CERBERUS_STORAGE_MOUNT_PATH "sdcard"
#include "recon_storage_types.h"

static unsigned checks, case_number;
#define CHECK(v) do { ++checks; if (!(v)) { fprintf(stderr,"Recon storage %u: %s\n",__LINE__,#v); exit(1); } } while(0)
#define TAG "recon-storage-check"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
typedef int *SemaphoreHandle_t;
static int mutex_value;
static SemaphoreHandle_t storage_mutex;
static bool locked, sd_mounted, fail_mutex_create, fail_mutex_take;
static cerberus_storage_owner_t storage_owner;
static FILE *sd_wifi_capture_file;
static uint64_t sd_wifi_capture_bytes;
static unsigned locks, releases, unmounts, finish_waits;
static FILE *streams[8];
static int descriptors[8];
static unsigned stream_count, descriptor_count;
enum event { STAT, MKDIR, OPEN, FDOPEN, WRITE, FLUSH, SYNC, CLOSE, RENAME, RAW_CLOSE, EVENT_COUNT };
static unsigned calls[EVENT_COUNT], fault_at[EVENT_COUNT];
static bool fail_all_closes, collide_candidates, collide_last, injected_ferror;
static unsigned raced_temp_at;
static char session[7];
static bool packet_count_testing;
static const char csv_header[]="kind,name\n", row[]="wifi,test\n", manifest[]="{\"complete\":true}\n";
static const char error_manifest[]="{\"state\":\"error\",\"error_code\":260}\n";

static bool event(enum event which)
{
    CHECK(locked && (sd_recon.active || packet_count_testing));
    ++calls[which];
    return fault_at[which] && calls[which]==fault_at[which];
}
static SemaphoreHandle_t xSemaphoreCreateMutex(void)
{return fail_mutex_create?NULL:&mutex_value;}
static int xSemaphoreTake(SemaphoreHandle_t mutex,unsigned wait)
{
    CHECK(mutex==&mutex_value && !locked && (wait==1000 || wait==portMAX_DELAY));
    if(wait==portMAX_DELAY)++finish_waits;
    if(fail_mutex_take && wait!=portMAX_DELAY)return 0;
    ++locks;locked=true;return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex)
{CHECK(mutex==&mutex_value && locked);locked=false;++releases;}
static void vTaskDelay(unsigned ticks) {CHECK(locked && ticks==1);}
static bool cerberus_storage_is_mounted(void) {return sd_mounted;}
static esp_err_t cerberus_storage_mount(void)
{return sd_mounted && storage_owner==CERBERUS_STORAGE_OWNER_FIRMWARE?ESP_OK:ESP_ERR_INVALID_STATE;}
static esp_err_t unmount_locked(void) {CHECK(locked);++unmounts;sd_mounted=false;return ESP_OK;}
static int64_t esp_timer_get_time(void) {return 1000;}

static int wrapped_stat(const char *path,struct stat *value)
{
    if(event(STAT)){errno=EIO;return -1;}
    const char *name=strrchr(path,'/');
    if(name && strlen(name)==7 && name[1]=='R' &&
        (collide_candidates || (collide_last && !strcmp(name,"/R99999")))) {
        memset(value,0,sizeof(*value));value->st_mode=S_IFDIR;return 0;
    }
    return stat(path,value);
}
static int wrapped_mkdir(const char *path,int mode)
{CHECK(mode==0775);if(event(MKDIR)){errno=EIO;return -1;}return native_mkdir(path);}
static int wrapped_open(const char *path,int flags,int mode)
{
    CHECK((sd_recon.active || sd_packet_count) && mode==0664 && flags==(O_WRONLY|O_CREAT|O_EXCL));
    if(event(OPEN)){errno=EIO;return -1;}
    if(raced_temp_at==calls[OPEN]){FILE *f=fopen(path,"wb");CHECK(f);CHECK(fwrite("KEEP",1,4,f)==4);CHECK(fclose(f)==0);}
    int descriptor=native_open(path,flags|O_BINARY,mode);
    if(descriptor>=0){CHECK(descriptor_count<8);descriptors[descriptor_count++]=descriptor;}
    return descriptor;
}
static FILE *wrapped_fdopen(int descriptor,const char *mode)
{
    CHECK(descriptor_count && !strcmp(mode,"wb"));
    if(event(FDOPEN)){errno=ENOMEM;return NULL;}
    FILE *stream=native_fdopen(descriptor,mode);
    if(stream){CHECK(descriptors[descriptor_count-1]==descriptor);--descriptor_count;streams[stream_count++]=stream;}
    return stream;
}
static int wrapped_close(int descriptor)
{
    bool fail=event(RAW_CLOSE);CHECK(descriptor_count && descriptors[descriptor_count-1]==descriptor);
    --descriptor_count;int result=native_close(descriptor);return fail?-1:result;
}
static size_t wrapped_write(const void *data,size_t size,size_t length,FILE *stream)
{
    CHECK(size==1 && length>0 && (sd_recon.active || sd_packet_count));
    if(event(WRITE))return length>1?fwrite(data,1,length/2,stream):0;
    return fwrite(data,size,length,stream);
}
static int wrapped_ferror(FILE *stream) {return injected_ferror || ferror(stream);}
static int wrapped_flush(FILE *stream)
{if(event(FLUSH)){errno=EIO;return EOF;}return fflush(stream);}
static int wrapped_sync(int descriptor)
{
    CHECK(descriptor>=0);
    if(event(SYNC)){errno=EIO;return -1;}
    return 0; /* Injected boundary; not host or SD durability evidence. */
}
static int wrapped_fclose(FILE *stream)
{
    bool fail=event(CLOSE)||fail_all_closes;
    unsigned index=0;while(index<stream_count && streams[index]!=stream)++index;
    CHECK(index<stream_count);streams[index]=streams[--stream_count];
    int result=fclose(stream);return fail?EOF:result;
}
static int wrapped_rename(const char *old,const char *new_name)
{
    CHECK(!stream_count && !descriptor_count);
    if(event(RENAME)){errno=EIO;return -1;}
    return rename(old,new_name);
}
static FILE *wrapped_fopen(const char *path,const char *mode)
{
    CHECK(locked);FILE *stream=fopen(path,mode);
    if(stream)streams[stream_count++]=stream;
    return stream;
}

#define stat(path,result) wrapped_stat(path,result)
#define mkdir wrapped_mkdir
#define open wrapped_open
#define fdopen wrapped_fdopen
#define close wrapped_close
#define fwrite wrapped_write
#define ferror wrapped_ferror
#define fflush wrapped_flush
#define fsync wrapped_sync
#define fileno native_fileno
#define fclose wrapped_fclose
#define rename wrapped_rename
#define fopen wrapped_fopen
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
static unsigned count_allocations;
static bool count_fail_allocation;
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps)
{CHECK(caps==3U);if(count_fail_allocation)return NULL;void *p=calloc(n,size);if(p)++count_allocations;return p;}
static void heap_caps_free(void *p) {CHECK(p&&count_allocations);--count_allocations;free(p);}
#include "recon_storage_impl.h"
#include "storage_packet_count_impl.h"
#undef stat
#undef mkdir
#undef open
#undef fdopen
#undef close
#undef fwrite
#undef ferror
#undef fflush
#undef fsync
#undef fileno
#undef fclose
#undef rename
#undef fopen

static void setup(void)
{
    CHECK(!locked && !stream_count && !descriptor_count && !sd_recon.active);
    if(case_number)CHECK(native_chdir("..")==0);
    char name[32];snprintf(name,sizeof(name),"case%04u",++case_number);
    CHECK(native_mkdir(name)==0 && native_chdir(name)==0 && native_mkdir("sdcard")==0);
    CHECK(!sd_packet_count&&!count_allocations);count_fail_allocation=false;sd_packet_count_next_index=1;
    memset(&sd_recon,0,sizeof(sd_recon));sd_recon_next_index=1;
    memset(&sd_wardrive_stream,0,sizeof(sd_wardrive_stream));memset(&sd_wigle_stream,0,sizeof(sd_wigle_stream));
    sd_wardrive_session_active=false;sd_wifi_capture_file=NULL;sd_wifi_capture_bytes=0;
    storage_mutex=&mutex_value;sd_mounted=true;storage_owner=CERBERUS_STORAGE_OWNER_FIRMWARE;
    fail_mutex_create=fail_mutex_take=fail_all_closes=collide_candidates=collide_last=injected_ferror=false;
    raced_temp_at=0;
    memset(calls,0,sizeof(calls));memset(fault_at,0,sizeof(fault_at));
    locks=releases=unmounts=finish_waits=0;session[0]='\0';
}
static void path(char *output,const char *name)
{snprintf(output,128,"sdcard/CERBERUS/RECON/%s/%s",session,name);}
static bool exists(const char *name)
{char file[128];path(file,name);struct stat item;return stat(file,&item)==0;}
static void data_equals(const char *name,const char *wanted,size_t size)
{
    char file[128],buffer[4096];path(file,name);FILE *stream=fopen(file,"rb");CHECK(stream);
    CHECK(size<=sizeof(buffer));CHECK(fread(buffer,1,sizeof(buffer),stream)==size);
    CHECK(!memcmp(buffer,wanted,size));CHECK(fgetc(stream)==EOF && !ferror(stream));CHECK(fclose(stream)==0);
}
static void create_data(const char *name,const char *data)
{char file[128];path(file,name);FILE *stream=fopen(file,"wb");CHECK(stream);CHECK(fwrite(data,1,strlen(data),stream)==strlen(data));CHECK(fclose(stream)==0);}
static esp_err_t begin(void)
{return cerberus_storage_recon_begin(csv_header,sizeof(csv_header)-1,session,sizeof(session));}
static esp_err_t finish(bool success)
{return cerberus_storage_recon_finish(manifest,sizeof(manifest)-1,success);}
static void released(void)
{CHECK(!sd_recon.active && !sd_recon.observations && !sd_recon.manifest && !locked && !stream_count && !descriptor_count);CHECK(locks==releases);}
static void fail_next(enum event kind) {fault_at[kind]=calls[kind]+1;}

static void normal_and_guards(void)
{
    setup();CHECK(begin()==ESP_OK && !strcmp(session,"R00001"));
    CHECK(sd_recon.active && stream_count==2 && !locked);
    CHECK(cerberus_storage_recon_append(row,sizeof(row)-1)==ESP_OK);
    CHECK(cerberus_storage_recon_sync()==ESP_OK);
    CHECK(cerberus_storage_reserve_for_usb()==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_storage_unmount()==ESP_ERR_INVALID_STATE && !unmounts);
    CHECK(cerberus_storage_wifi_capture_begin((const uint8_t *)"h",1)==ESP_ERR_INVALID_STATE);
    CHECK(wardrive_stream_begin(&sd_wardrive_stream,"UNLOC","h",1)==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_storage_wardrive_session_begin()==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_storage_recon_begin(csv_header,sizeof(csv_header)-1,session,sizeof(session))==ESP_ERR_INVALID_STATE);
    strcpy(session,"R00001");
    const char *aliases[]={"sdcard/CERBERUS/RECON/R00001", "sdcard/cerberus/recon/r00001/OBS.TMP",
        "sdcard//CERBERUS/./RECON/R00001/SESSION.TMP", "sdcard/CERBERUS./RECON /R00001./OBS.TMP",
        "sdcard/CERBERUS", "sdcard/CERBERUS/RECON"};
    for(unsigned i=0;i<sizeof(aliases)/sizeof(aliases[0]);++i)CHECK(recon_path_protected(aliases[i]));
    CHECK(!recon_path_protected("sdcard/CERBERUS/RECON/R00002/OBS.TMP"));
    CHECK(!recon_path_protected("sdcard/CERBERUS/RECONSTRUCTION"));
    unsigned before=calls[STAT];
    CHECK(cerberus_storage_delete_entry("/CERBERUS/RECON","R00001")==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_storage_delete_entry("/","CERBERUS.")==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_storage_move_entry("/CERBERUS/RECON","R00001","/","moved")==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_storage_move_entry("/","elsewhere","/CERBERUS/RECON/R00001","moved")==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_storage_create_directory("/CERBERUS/RECON/R00001","child")==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_storage_create_empty_file("/CERBERUS/RECON/R00001","new")==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_storage_copy_file("/","source","/CERBERUS/RECON/R00001","new")==ESP_ERR_INVALID_STATE);
    CHECK(calls[STAT]==before);
    CHECK(finish(true)==ESP_OK);released();CHECK(finish_waits==1 && calls[RENAME]==2);
    CHECK(!exists("OBS.TMP") && !exists("SESSION.TMP"));
    data_equals("OBS.CSV","kind,name\nwifi,test\n",20);
    data_equals("SESSION.JSON",manifest,sizeof(manifest)-1);
    CHECK(cerberus_storage_reserve_for_usb()==ESP_OK && unmounts==1);
    CHECK(storage_owner==CERBERUS_STORAGE_OWNER_USB);
}

static void admission_and_begin_failures(void)
{
    setup();CHECK(cerberus_storage_recon_begin(NULL,1,session,sizeof(session))==ESP_ERR_INVALID_ARG);
    CHECK(cerberus_storage_recon_begin(csv_header,1025,session,sizeof(session))==ESP_ERR_INVALID_ARG);
    CHECK(cerberus_storage_recon_begin(csv_header,1,session,6)==ESP_ERR_INVALID_ARG);CHECK(!locks);
    sd_mounted=false;CHECK(begin()==ESP_ERR_INVALID_STATE);sd_mounted=true;
    storage_owner=CERBERUS_STORAGE_OWNER_USB;CHECK(begin()==ESP_ERR_INVALID_STATE);storage_owner=CERBERUS_STORAGE_OWNER_FIRMWARE;
    sd_wifi_capture_file=(FILE *)(uintptr_t)1;CHECK(begin()==ESP_ERR_INVALID_STATE);sd_wifi_capture_file=NULL;
    sd_wardrive_session_active=true;CHECK(begin()==ESP_ERR_INVALID_STATE);sd_wardrive_session_active=false;
    sd_wigle_stream.file=(FILE *)(uintptr_t)1;CHECK(begin()==ESP_ERR_INVALID_STATE);sd_wigle_stream.file=NULL;
    sd_gps=(gps_storage_t *)(uintptr_t)1;CHECK(begin()==ESP_ERR_INVALID_STATE);sd_gps=NULL;
    storage_mutex=NULL;fail_mutex_create=true;CHECK(begin()==ESP_ERR_NO_MEM);fail_mutex_create=false;
    fail_mutex_take=true;CHECK(begin()==ESP_ERR_TIMEOUT);fail_mutex_take=false;released();
    const enum event events[]={STAT,MKDIR,OPEN,FDOPEN,WRITE,FLUSH,SYNC};
    for(unsigned i=0;i<sizeof(events)/sizeof(events[0]);++i) {
        unsigned maximum=(events[i]==STAT?5:events[i]==MKDIR?3:events[i]==OPEN||events[i]==FDOPEN?2:1);
        for(unsigned nth=1;nth<=maximum;++nth) {
            setup();fault_at[events[i]]=nth;CHECK(begin()!=ESP_OK);released();
            CHECK(calls[RENAME]==0);
            if(session[0])CHECK(!exists("OBS.CSV") && !exists("SESSION.JSON"));
            if(events[i]==FDOPEN)CHECK(calls[RAW_CLOSE]==1);
        }
    }
    setup();fault_at[WRITE]=1;fail_all_closes=true;CHECK(begin()!=ESP_OK);released();CHECK(calls[CLOSE]==2 && session[0]);
    setup();raced_temp_at=1;CHECK(begin()!=ESP_OK);released();data_equals("OBS.TMP","KEEP",4);
    setup();raced_temp_at=2;CHECK(begin()!=ESP_OK);released();data_equals("SESSION.TMP","KEEP",4);
    setup();CHECK(native_mkdir("sdcard/CERBERUS")==0 && native_mkdir("sdcard/CERBERUS/RECON")==0);
    CHECK(native_mkdir("sdcard/CERBERUS/RECON/R00001")==0);strcpy(session,"R00001");create_data("OBS.TMP","HISTORY");
    CHECK(begin()==ESP_OK && !strcmp(session,"R00002"));CHECK(finish(false)!=ESP_OK);released();
    strcpy(session,"R00001");data_equals("OBS.TMP","HISTORY",7);
    setup();collide_candidates=true;CHECK(begin()==ESP_ERR_NOT_FOUND);released();
    CHECK(sd_recon_next_index==1025 && calls[STAT]==1026 && !session[0]);
    setup();sd_recon_next_index=99999;collide_last=true;CHECK(begin()==ESP_OK && !strcmp(session,"R00001"));
    CHECK(sd_recon_next_index==2);CHECK(finish(false)!=ESP_OK);released();
}

static void append_and_sync_failures(void)
{
    setup();char maximum_row[CERBERUS_STORAGE_RECON_ROW_MAX], maximum_manifest[CERBERUS_STORAGE_RECON_MANIFEST_MAX];
    memset(maximum_row,'R',sizeof(maximum_row));memset(maximum_manifest,'M',sizeof(maximum_manifest));
    CHECK(cerberus_storage_recon_begin(maximum_row,sizeof(maximum_row),session,sizeof(session))==ESP_OK);
    CHECK(cerberus_storage_recon_append(maximum_row,sizeof(maximum_row))==ESP_OK);
    CHECK(cerberus_storage_recon_finish(maximum_manifest,sizeof(maximum_manifest),true)==ESP_OK);released();
    char expected_rows[2*CERBERUS_STORAGE_RECON_ROW_MAX];memset(expected_rows,'R',sizeof(expected_rows));
    data_equals("OBS.CSV",expected_rows,sizeof(expected_rows));data_equals("SESSION.JSON",maximum_manifest,sizeof(maximum_manifest));
    for(unsigned kind=0;kind<8;++kind) {
        setup();CHECK(begin()==ESP_OK);esp_err_t error;
        if(kind==0){fail_next(WRITE);error=cerberus_storage_recon_append(row,sizeof(row)-1);}
        else if(kind==1){injected_ferror=true;error=cerberus_storage_recon_append(row,sizeof(row)-1);injected_ferror=false;}
        else if(kind==2)error=cerberus_storage_recon_append(NULL,1);
        else if(kind==3)error=cerberus_storage_recon_append(row,0);
        else if(kind==4)error=cerberus_storage_recon_append(row,1025);
        else if(kind==5){sd_recon.bytes=CERBERUS_STORAGE_RECON_MAX_BYTES-CERBERUS_STORAGE_RECON_MANIFEST_MAX-2;error=cerberus_storage_recon_append(row,3);}
        else {fail_next(kind==6?FLUSH:SYNC);error=cerberus_storage_recon_sync();}
        CHECK(error!=ESP_OK && sd_recon.active);
        unsigned writes=calls[WRITE];CHECK(cerberus_storage_recon_append(row,sizeof(row)-1)!=ESP_OK);CHECK(calls[WRITE]==writes);
        CHECK((kind==5?cerberus_storage_recon_finish(error_manifest,sizeof(error_manifest)-1,false):finish(true))!=ESP_OK);
        released();CHECK(!exists("OBS.CSV") && !exists("SESSION.JSON"));
        CHECK(exists("OBS.TMP") && exists("SESSION.TMP"));
        if(kind==5)data_equals("SESSION.TMP",error_manifest,sizeof(error_manifest)-1);
    }
    setup();CHECK(begin()==ESP_OK);fail_mutex_take=true;
    CHECK(cerberus_storage_recon_append(row,sizeof(row)-1)==ESP_ERR_TIMEOUT);
    CHECK(finish(false)!=ESP_OK);released();CHECK(finish_waits==1);
    setup();CHECK(begin()==ESP_OK);sd_recon.bytes=CERBERUS_STORAGE_RECON_MAX_BYTES-(sizeof(manifest)-1);
    CHECK(finish(true)==ESP_OK);released();
    setup();CHECK(begin()==ESP_OK);
    sd_recon.bytes=CERBERUS_STORAGE_RECON_MAX_BYTES-CERBERUS_STORAGE_RECON_MANIFEST_MAX-1;
    CHECK(cerberus_storage_recon_append(row,1)==ESP_OK);
    CHECK(cerberus_storage_recon_append(row,1)==ESP_ERR_INVALID_SIZE);
    CHECK(cerberus_storage_recon_finish(maximum_manifest,sizeof(maximum_manifest),true)==ESP_ERR_INVALID_SIZE);
    released();CHECK(!exists("SESSION.JSON"));data_equals("SESSION.TMP",maximum_manifest,sizeof(maximum_manifest));
    setup();CHECK(begin()==ESP_OK);sd_recon.bytes=CERBERUS_STORAGE_RECON_MAX_BYTES;
    CHECK(finish(true)==ESP_ERR_INVALID_SIZE);released();CHECK(!exists("SESSION.JSON"));
}

static void finish_failures(void)
{
    const enum event events[]={WRITE,FLUSH,SYNC,CLOSE,RENAME,STAT};
    for(unsigned i=0;i<sizeof(events)/sizeof(events[0]);++i) {
        unsigned maximum=events[i]==WRITE?1:2;
        for(unsigned nth=1;nth<=maximum;++nth) {
            setup();CHECK(begin()==ESP_OK);fault_at[events[i]]=calls[events[i]]+nth;
            CHECK(finish(true)!=ESP_OK);released();CHECK(calls[CLOSE]==2);
            CHECK(!exists("SESSION.JSON") && exists("SESSION.TMP"));
            bool first_published=events[i]==RENAME && nth==2;
            CHECK(exists("OBS.CSV")==first_published);CHECK(exists("OBS.TMP")!=first_published);
            CHECK(calls[RENAME]<3);
        }
    }
    setup();CHECK(begin()==ESP_OK);fail_all_closes=true;CHECK(finish(true)!=ESP_OK);released();CHECK(calls[CLOSE]==2 && calls[RENAME]==0);
    setup();CHECK(begin()==ESP_OK);CHECK(cerberus_storage_recon_finish(NULL,0,true)==ESP_ERR_INVALID_ARG);released();
    setup();CHECK(begin()==ESP_OK);CHECK(cerberus_storage_recon_finish(manifest,4097,true)==ESP_ERR_INVALID_ARG);released();
    setup();CHECK(begin()==ESP_OK);create_data("OBS.CSV","UNCHANGED");CHECK(finish(true)==ESP_ERR_INVALID_STATE);released();data_equals("OBS.CSV","UNCHANGED",9);
    setup();CHECK(begin()==ESP_OK);create_data("SESSION.JSON","UNCHANGED");CHECK(finish(true)==ESP_ERR_INVALID_STATE);released();data_equals("SESSION.JSON","UNCHANGED",9);
    setup();CHECK(begin()==ESP_OK);CHECK(finish(false)!=ESP_OK);released();
    data_equals("SESSION.TMP",manifest,sizeof(manifest)-1);CHECK(!exists("SESSION.JSON"));
    CHECK(cerberus_storage_recon_append(row,1)==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_storage_recon_sync()==ESP_ERR_INVALID_STATE);
    CHECK(finish(true)==ESP_ERR_INVALID_STATE);released();
}

#include "wifi_packet_count_storage_cases.h"

int main(void)
{
    normal_and_guards();admission_and_begin_failures();append_and_sync_failures();finish_failures();packet_count_storage_cases();
    printf("Recon journal storage PASS %u checks across %u real-file cases\n",checks,case_number);
    return 0;
}
