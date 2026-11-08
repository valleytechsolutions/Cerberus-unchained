/* Private storage implementation: included after the mutex/path helpers. */
#include "wardrive_log_storage.h"
static unsigned ward_log_open_files;

static bool ward_log_token_valid(uint64_t token)
{ return token && token == sd_ward_log_lease && require_firmware_owner_locked() == ESP_OK; }

esp_err_t cerberus_ward_log_storage_begin(uint64_t *token)
{
    if (!token) return ESP_ERR_INVALID_ARG;
    *token = 0;
    ESP_RETURN_ON_ERROR(take_storage_mutex(), TAG, "log lease lock");
    esp_err_t err = require_firmware_owner_locked();
    if (err == ESP_OK && (wardrive_storage_busy() || sd_wifi_capture_file || sd_recon.active ||
        sd_packet_count || sd_ward_log_sequence == UINT64_MAX)) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) *token = sd_ward_log_lease = ++sd_ward_log_sequence;
    release_storage_mutex(); return err;
}

esp_err_t cerberus_ward_log_storage_end(uint64_t token)
{
    ESP_RETURN_ON_ERROR(take_storage_mutex(), TAG, "log release lock");
    esp_err_t err = ward_log_token_valid(token) && !ward_log_open_files ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) sd_ward_log_lease = 0;
    release_storage_mutex(); return err;
}

static unsigned ward_log_group(const char *name)
{
    if (!strncmp(name, CERBERUS_WARD_LOG_PREFIX, sizeof(CERBERUS_WARD_LOG_PREFIX)-1U)) return 0;
    return cerberus_ward_log_name(name)==CERBERUS_WARD_LOG_WIGLE ? 1U : 2U;
}
static int ward_log_compare(const void *a, const void *b)
{
    const char *left=((const cerberus_ward_log_entry_t *)a)->name;
    const char *right=((const cerberus_ward_log_entry_t *)b)->name;
    unsigned ga=ward_log_group(left),gb=ward_log_group(right);
    return ga!=gb ? (ga<gb ? -1 : 1) : strcmp(right,left);
}

esp_err_t cerberus_ward_log_storage_list(uint64_t token, cerberus_ward_log_entry_t *entries,
    size_t capacity, size_t *count)
{ return cerberus_ward_log_storage_list_info(token,entries,capacity,count,NULL); }

esp_err_t cerberus_ward_log_storage_list_info(uint64_t token, cerberus_ward_log_entry_t *entries,
    size_t capacity, size_t *count, cerberus_ward_log_catalog_info_t *info)
{
    if (info) memset(info,0,sizeof(*info));
    if (!entries || !count || capacity < CERBERUS_WARD_LOG_CAPACITY) return ESP_ERR_INVALID_ARG;
    *count = 0;
    ESP_RETURN_ON_ERROR(take_storage_mutex(), TAG, "log list lock");
    esp_err_t err = ward_log_token_valid(token) ? ESP_OK : ESP_ERR_INVALID_STATE;
    DIR *dir = NULL;
    if (err == ESP_OK) {
        dir = opendir(CERBERUS_STORAGE_MOUNT_PATH CERBERUS_STORAGE_WARDRIVE_PATH);
        if (!dir && errno != ENOENT) err = ESP_FAIL;
    }
    if (dir) {
        if (info) info->directory_present=true;
        struct dirent *entry;
        for (;;) {
            errno = 0; entry = readdir(dir);
            if (!entry) { if (errno) err = ESP_FAIL; break; }
            cerberus_ward_log_kind_t kind=cerberus_ward_log_name(entry->d_name);
            if (info && strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")) {
                if(info->entries<UINT32_MAX)++info->entries;
                size_t length=strlen(entry->d_name);
                const char *suffix=length>=4?entry->d_name+length-4:"";
                bool csv=length>=4&&suffix[0]=='.'&&(suffix[1]=='c'||suffix[1]=='C')&&
                    (suffix[2]=='s'||suffix[2]=='S')&&(suffix[3]=='v'||suffix[3]=='V');
                bool tmp=length>=4&&suffix[0]=='.'&&(suffix[1]=='t'||suffix[1]=='T')&&
                    (suffix[2]=='m'||suffix[2]=='M')&&(suffix[3]=='p'||suffix[3]=='P');
                if(csv&&info->csv_entries<UINT32_MAX)++info->csv_entries;
                if(tmp&&info->temporary_entries<UINT32_MAX)++info->temporary_entries;
                if(csv&&!kind){
                    if(info->unrecognized_csv<UINT32_MAX)++info->unrecognized_csv;
                    if(!info->first_unrecognized_csv[0])
                        snprintf(info->first_unrecognized_csv,sizeof(info->first_unrecognized_csv),"%.*s",
                            (int)sizeof(info->first_unrecognized_csv)-1,entry->d_name);
                }
            }
            if (!kind) continue;
            char path[96]; struct stat st;
            snprintf(path, sizeof(path), "%s%s/%.*s", CERBERUS_STORAGE_MOUNT_PATH,
                CERBERUS_STORAGE_WARDRIVE_PATH, (int)CERBERUS_WARD_LOG_NAME_CAPACITY-1, entry->d_name);
            if (stat(path, &st)) { err = ESP_FAIL; break; }
            if (!S_ISREG(st.st_mode)) continue;
            if (*count == capacity || st.st_size < 0) { err = ESP_ERR_INVALID_SIZE; break; }
            cerberus_ward_log_entry_t *out = &entries[(*count)++];
            memset(out, 0, sizeof(*out)); memcpy(out->name, entry->d_name, strlen(entry->d_name)+1U);
            out->bytes = (uint64_t)st.st_size;
        }
        if (closedir(dir)) err = ESP_FAIL;
    }
    if (err == ESP_OK) qsort(entries, *count, sizeof(*entries), ward_log_compare);
    else *count = 0;
    release_storage_mutex(); return err;
}

esp_err_t cerberus_ward_log_storage_open(uint64_t token, const char *name, FILE **file)
{
    if (!file || !cerberus_ward_log_name(name)) return ESP_ERR_INVALID_ARG;
    *file = NULL;
    ESP_RETURN_ON_ERROR(take_storage_mutex(), TAG, "log open lock");
    esp_err_t err = ward_log_token_valid(token) && !ward_log_open_files ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        char path[96]; snprintf(path, sizeof(path), "%s%s/%s", CERBERUS_STORAGE_MOUNT_PATH,
            CERBERUS_STORAGE_WARDRIVE_PATH, name);
        *file = fopen(path, "rb");
        if (!*file) err = ESP_FAIL; else ++ward_log_open_files;
    }
    release_storage_mutex(); return err;
}

esp_err_t cerberus_ward_log_storage_close(uint64_t token, FILE **file)
{
    if (!file || !*file) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(take_storage_mutex(), TAG, "log close lock");
    esp_err_t err = ward_log_token_valid(token) && ward_log_open_files ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) { int rc = fclose(*file); *file = NULL; --ward_log_open_files; if (rc) err = ESP_FAIL; }
    release_storage_mutex(); return err;
}

esp_err_t cerberus_ward_log_storage_delete(uint64_t token, const char *name, uint64_t expected_bytes)
{
    if (!cerberus_ward_log_name(name)) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(take_storage_mutex(), TAG, "log delete lock");
    esp_err_t err = ward_log_token_valid(token) && !ward_log_open_files ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        char path[96]; struct stat st;
        snprintf(path, sizeof(path), "%s%s/%s", CERBERUS_STORAGE_MOUNT_PATH, CERBERUS_STORAGE_WARDRIVE_PATH, name);
        if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size != expected_bytes)
            err = ESP_ERR_INVALID_STATE;
        else if (remove(path)) err = ESP_FAIL;
    }
    release_storage_mutex(); return err;
}

static bool ward_receipt_key(const char *key)
{
    if (!key) return false;
    for (unsigned i = 0; i < 68; ++i) {
        unsigned char c = key[i];
        if (i < 64 ? !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) :
            i == 64 ? c != '-' : !(c >= '0' && c <= '9')) return false;
    }
    return key[68] == 0;
}

esp_err_t cerberus_ward_log_profile_read(uint64_t token,void *out,size_t capacity,size_t *bytes)
{
    if(!out||!bytes||!capacity||capacity>4096)return ESP_ERR_INVALID_ARG;
    *bytes=0;
    ESP_RETURN_ON_ERROR(take_storage_mutex(),TAG,"profile read lock");
    esp_err_t err=ward_log_token_valid(token)&&!ward_log_open_files?ESP_OK:ESP_ERR_INVALID_STATE;
    if(err==ESP_OK){
        const char *path=CERBERUS_STORAGE_MOUNT_PATH CERBERUS_WARD_UPLOAD_PROFILE_PATH;
        struct stat st;
        if(stat(path,&st))err=errno==ENOENT?ESP_ERR_NOT_FOUND:ESP_FAIL;
        else if(!S_ISREG(st.st_mode)||st.st_size<=0||(uint64_t)st.st_size>capacity)err=ESP_ERR_INVALID_SIZE;
        else{
            FILE *file=fopen(path,"rb");
            if(!file)err=ESP_FAIL;
            else{
                size_t n=fread(out,1,capacity,file);
                if(n!=(size_t)st.st_size||fgetc(file)!=EOF||ferror(file))err=ESP_FAIL;
                if(fclose(file))err=ESP_FAIL;
                if(err==ESP_OK)*bytes=n;
            }
        }
    }
    release_storage_mutex();return err;
}
esp_err_t cerberus_ward_log_profile_remove(uint64_t token,size_t expected_bytes)
{
    if(!expected_bytes||expected_bytes>4096)return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(take_storage_mutex(),TAG,"profile remove lock");
    esp_err_t err=ward_log_token_valid(token)&&!ward_log_open_files?ESP_OK:ESP_ERR_INVALID_STATE;
    if(err==ESP_OK){
        const char *path=CERBERUS_STORAGE_MOUNT_PATH CERBERUS_WARD_UPLOAD_PROFILE_PATH;
        struct stat st;
        if(stat(path,&st)||!S_ISREG(st.st_mode)||st.st_size!=(long)expected_bytes)err=ESP_ERR_INVALID_STATE;
        else if(remove(path))err=ESP_FAIL;
    }
    release_storage_mutex();return err;
}

static esp_err_t ward_receipt(uint64_t token, const char *key, void *data, size_t bytes, bool write)
{
    if (!ward_receipt_key(key) || !data || !bytes || bytes > 512) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(take_storage_mutex(), TAG, "receipt lock");
    esp_err_t err = ward_log_token_valid(token) ? ESP_OK : ESP_ERR_INVALID_STATE;
    char path[128];
    snprintf(path, sizeof(path), "%s%s/UPLOADS/%s.RCT", CERBERUS_STORAGE_MOUNT_PATH, CERBERUS_STORAGE_WARDRIVE_PATH, key);
    if (err == ESP_OK && write) err = ensure_directory_exists(CERBERUS_STORAGE_MOUNT_PATH CERBERUS_STORAGE_WARDRIVE_PATH "/UPLOADS");
    if (err == ESP_OK) {
        struct stat st;
        if (stat(path, &st) == 0) {
            if(st.st_size<0||(size_t)st.st_size%bytes||(size_t)st.st_size>32U*bytes||
                (write&&(size_t)st.st_size==32U*bytes))err=ESP_ERR_INVALID_STATE;
        } else if(errno!=ENOENT)err=ESP_FAIL;
    }
    if (err == ESP_OK) {
        FILE *f = fopen(path, write ? "ab" : "rb");
        if (!f) err = !write && errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        else {
            if (write) {
                if (fwrite(data, 1, bytes, f) != bytes || fflush(f) || fsync(fileno(f))) err = ESP_FAIL;
            } else if (fseek(f,-(long)bytes,SEEK_END) || fread(data, 1, bytes, f) != bytes ||
                fgetc(f) != EOF || ferror(f)) err = ESP_ERR_INVALID_RESPONSE;
            if (fclose(f)) err = ESP_FAIL;
        }
    }
    release_storage_mutex(); return err;
}
esp_err_t cerberus_ward_log_receipt_read(uint64_t token, const char *key, void *out, size_t bytes)
{ return ward_receipt(token, key, out, bytes, false); }
esp_err_t cerberus_ward_log_receipt_write(uint64_t token, const char *key, const void *data, size_t bytes)
{ return ward_receipt(token, key, (void *)data, bytes, true); }
