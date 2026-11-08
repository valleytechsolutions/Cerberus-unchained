#include "ssid_framebuffer.h"
#include "ssid_list_session.h"
#include "sentinel_tools.h"
#define UI_BACKGROUND 0x0042
#define UI_PANEL 0x08A4
#define UI_MUTED 0x7CB5
#define UI_TEXT 0xE77F
#define UI_BAD 0xF471
#define UI_GOOD 0x471B
#define TAG "ssid-ui"
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)
static bool privacy,hold,poll_error;
static bool privacy_addresses_masked(void) { return privacy; }
static const char *esp_err_to_name(esp_err_t e) { (void)e; return "ESP_FAIL"; }
static esp_err_t draw_centered(uint16_t y,const char *s,uint16_t color,uint8_t scale)
{ CHECK(strlen(s)*6U*scale<=320U);return cerberus_display_draw_text((320U-strlen(s)*6U*scale)/2U,y,s,color,UI_BACKGROUND,scale); }
static esp_err_t draw_header(const char *s) { return draw_centered(20,s,UI_TEXT,2); }
static esp_err_t draw_small_button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char *s)
{ CHECK(strlen(s)*6U<=w);esp_err_t e=cerberus_display_fill_rect(x,y,w,h,UI_PANEL);
    return e?e:cerberus_display_draw_text(x+(w-strlen(s)*6U)/2U,y+(h-7U)/2U,s,UI_TEXT,UI_PANEL,1); }
static cerberus_ssid_list_t work;
static cerberus_wifi_marauder_ssid_t imported,saved;
static cerberus_ssid_list_session_status_t io;
static unsigned opens,edits,saves,closes,entropy_calls;
static esp_err_t map(cerberus_ssid_list_result_t r)
{ io.model_result=r;return r==CERBERUS_SSID_LIST_OK?ESP_OK:r==CERBERUS_SSID_LIST_LIMIT?ESP_ERR_INVALID_SIZE:
    r==CERBERUS_SSID_LIST_UNREPRESENTABLE?ESP_ERR_NOT_SUPPORTED:ESP_ERR_INVALID_STATE; }
void esp_fill_random(void *data,size_t length)
{ ++entropy_calls;uint8_t *p=data;for(size_t i=0;i<length;++i)p[i]=(uint8_t)(i+entropy_calls); }
esp_err_t cerberus_ssid_list_session_open(void)
{ ++opens;io.allocated=true;io.closing=false;return ESP_OK; }
esp_err_t cerberus_ssid_list_session_copy(cerberus_ssid_list_t *out) { *out=work;return ESP_OK; }
esp_err_t cerberus_ssid_list_session_edit(uint64_t revision,const cerberus_ssid_list_edit_t *edit)
{
    ++edits;if(revision!=work.revision||io.closing)return ESP_ERR_INVALID_STATE;
    switch(edit->kind){
    case CERBERUS_SSID_EDIT_ADD:return map(cerberus_ssid_list_add(&work,edit->bytes,edit->length));
    case CERBERUS_SSID_EDIT_NAME:return map(cerberus_ssid_list_edit(&work,edit->index,edit->bytes,edit->length));
    case CERBERUS_SSID_EDIT_REMOVE:return map(cerberus_ssid_list_remove(&work,edit->index));
    case CERBERUS_SSID_EDIT_REMOVE_SELECTED:return map(cerberus_ssid_list_remove_selected(&work));
    case CERBERUS_SSID_EDIT_CLEAR:return map(cerberus_ssid_list_clear(&work));
    case CERBERUS_SSID_EDIT_SELECT:return map(cerberus_ssid_list_select(&work,edit->index,edit->selected));
    case CERBERUS_SSID_EDIT_SELECT_ALL:return map(cerberus_ssid_list_select_all(&work,edit->selected));
    case CERBERUS_SSID_EDIT_GENERATE:return map(cerberus_ssid_list_generate(&work,edit->count,edit->bytes,edit->length));
    }return ESP_ERR_INVALID_ARG;
}
esp_err_t cerberus_ssid_list_session_prepare_document(const cerberus_wifi_marauder_ssid_t *document)
{ imported=*document;++io.preview_id;io.preview_count=imported.count;io.preview_ready=true;io.state=CERBERUS_SSID_SESSION_PREVIEW;return ESP_OK; }
esp_err_t cerberus_ssid_list_session_copy_preview(cerberus_wifi_marauder_ssid_t *out,uint64_t *id)
{ if(!io.preview_ready)return ESP_ERR_INVALID_STATE;*out=imported;*id=io.preview_id;return ESP_OK; }
void cerberus_ssid_list_session_discard_preview(void) { io.preview_ready=false;io.preview_count=0; }
esp_err_t cerberus_ssid_list_session_apply_preview(uint64_t id,uint64_t revision,bool append,bool unique)
{
    if(id!=io.preview_id||revision!=work.revision||!io.preview_ready)return ESP_ERR_INVALID_STATE;
    esp_err_t e=map(append?cerberus_ssid_list_append(&work,&imported,unique):cerberus_ssid_list_replace(&work,&imported));
    if(e==ESP_OK)cerberus_ssid_list_session_discard_preview();return e;
}
esp_err_t cerberus_ssid_list_session_save(uint64_t revision)
{
    if(revision!=work.revision||io.busy)return ESP_ERR_INVALID_STATE;
    ++saves;CHECK(cerberus_ssid_list_copy(&work,&saved,&io.saved_revision)==CERBERUS_SSID_LIST_OK);
    io.busy=true;io.state=CERBERUS_SSID_SESSION_SAVING;return ESP_OK;
}
esp_err_t cerberus_ssid_list_session_poll(cerberus_ssid_list_session_status_t *out)
{
    if(poll_error)return ESP_ERR_TIMEOUT;
    if(io.busy&&!hold){io.busy=false;io.state=io.closing?CERBERUS_SSID_SESSION_CANCELED:CERBERUS_SSID_SESSION_COMPLETE;
        if(!io.closing){(void)cerberus_ssid_list_saved(&work,io.saved_revision);strcpy(io.file_name,"S00001.TXT");}}
    io.revision=work.revision;io.count=work.document.count;io.selected=work.selected;io.dirty=work.dirty;
    if(out)*out=io;return ESP_OK;
}
void cerberus_ssid_list_session_close(void)
{ ++closes;io.closing=true;cerberus_ssid_list_session_discard_preview(); }
#include "ssid_list_ui.h"
static ssid_list_ui_store_t view;
static void paint(void)
{ calls=drawn_size=0;drawn[0]=0;CHECK(ssid_list_ui_draw(&view)==ESP_OK); }
static ssid_list_ui_action_t tap(unsigned x,unsigned y)
{ ssid_list_ui_action_t a=ssid_list_ui_tap(&view,(uint16_t)x,(uint16_t)y);if(a==SSID_UI_REDRAW)paint();return a; }
static void reset(void)
{ memset(&io,0,sizeof(io));cerberus_ssid_list_init(&work);privacy=hold=poll_error=false;fail_after=0;
    ssid_list_ui_init(&view);paint(); }
static void seed(unsigned count)
{ for(unsigned i=0;i<count;++i){char name[33];snprintf(name,sizeof(name),"Lab name %02u",i);
    CHECK(cerberus_ssid_list_add(&work,(const uint8_t*)name,strlen(name))==CERBERUS_SSID_LIST_OK);}paint(); }
static void preview(const char *dir,const char *name)
{ char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",dir,name);FILE *f=fopen(path,"wb");CHECK(f);
    fprintf(f,"P6\n320 480\n255\n");for(size_t i=0;i<sizeof(screen)/sizeof(*screen);++i){uint16_t p=screen[i];
        uint8_t rgb[]={(uint8_t)((p>>11)*255/31),(uint8_t)(((p>>5)&63)*255/63),(uint8_t)((p&31)*255/31)};
        assert(fwrite(rgb,1,3,f)==3);}CHECK(!fclose(f)); }
static void hex_key(unsigned value)
{ CHECK(value<16);CHECK(tap(14+value%4*76,186+value/4*40)==SSID_UI_REDRAW); }
static void editing(const char *dir)
{
    reset();preview(dir,"empty");CHECK(tap(20,360)==SSID_UI_REDRAW&&view.view==SSID_UI_TEXT);
    CHECK(tap(14,120)==SSID_UI_REDRAW);CHECK(!strcmp(view.text,"1"));preview(dir,"text");
    CHECK(tap(240,360)==SSID_UI_REDRAW);CHECK(work.document.count==1&&work.document.rows[0].bytes[0]=='1');
    CHECK(tap(60,120)==SSID_UI_REDRAW&&view.view==SSID_UI_DETAIL);preview(dir,"detail");
    CHECK(tap(170,270)==SSID_UI_REDRAW&&view.view==SSID_UI_HEX);
    CHECK(tap(120,360)==SSID_UI_REDRAW);hex_key(0);hex_key(0);hex_key(0);hex_key(13);hex_key(15);hex_key(15);
    preview(dir,"hex");CHECK(tap(120,440)==SSID_UI_REDRAW);CHECK(work.document.rows[0].length==3);
    CHECK(work.document.rows[0].bytes[0]==0&&work.document.rows[0].bytes[1]==13&&work.document.rows[0].bytes[2]==255);
    CHECK(tap(60,120)==SSID_UI_REDRAW);CHECK(tap(20,270)==SSID_UI_REDRAW&&view.view==SSID_UI_HEX);
    CHECK(strstr(view.message,"BINARY"));CHECK(tap(120,360)==SSID_UI_REDRAW);hex_key(0);hex_key(10);
    uint64_t revision=work.revision;CHECK(tap(120,440)==SSID_UI_REDRAW);CHECK(work.revision==revision&&strstr(view.message,"0A"));
    CHECK(tap(120,360)==SSID_UI_REDRAW);hex_key(15);CHECK(tap(120,440)==SSID_UI_REDRAW&&strstr(view.message,"TWO HEX"));
    CHECK(tap(10,440)==SSID_UI_REDRAW&&view.view==SSID_UI_DETAIL&&work.revision==revision);
    CHECK(tap(10,440)==SSID_UI_REDRAW);CHECK(tap(20,360)==SSID_UI_REDRAW);CHECK(tap(220,440)==SSID_UI_REDRAW);
    CHECK(work.document.count==2&&work.document.rows[1].length==0);
    CHECK(tap(20,360)==SSID_UI_REDRAW);CHECK(tap(120,440)==SSID_UI_REDRAW&&view.view==SSID_UI_HEX);
    for(unsigned i=0;i<64;++i)hex_key(i%16);CHECK(view.hex_length==64);hex_key(1);CHECK(view.hex_length==64);
    CHECK(tap(10,440)==SSID_UI_REDRAW&&work.document.count==2);
}
static void list_controls(const char *dir)
{
    reset();seed(64);CHECK(view.painted_count==4);preview(dir,"full-list");
    for(unsigned i=0;i<15;++i)CHECK(tap(250,318)==SSID_UI_REDRAW);
    CHECK(view.painted[3]==63);CHECK(tap(20,270)==SSID_UI_REDRAW&&work.selected==(UINT64_C(1)<<63));
    CHECK(tap(120,440)==SSID_UI_REDRAW&&view.view==SSID_UI_OPTIONS);preview(dir,"options");
    CHECK(tap(20,110)==SSID_UI_REDRAW&&work.selected==UINT64_MAX);
    CHECK(tap(170,110)==SSID_UI_REDRAW&&!work.selected);
    CHECK(tap(90,330)==SSID_UI_REDRAW&&work.document.count==64&&view.error==ESP_ERR_INVALID_SIZE);
    CHECK(tap(20,110)==SSID_UI_REDRAW);CHECK(tap(20,170)==SSID_UI_REDRAW&&view.view==SSID_UI_CONFIRM);preview(dir,"confirm");
    CHECK(tap(40,270)==SSID_UI_REDRAW&&!work.document.count);
    CHECK(tap(120,440)==SSID_UI_REDRAW);CHECK(tap(250,330)==SSID_UI_REDRAW&&view.generate_count==2);
    CHECK(tap(90,330)==SSID_UI_REDRAW&&work.document.count==2&&entropy_calls);CHECK(!memcmp(work.document.rows[0].bytes,"CERB-",5));
    CHECK(tap(20,230)==SSID_UI_REDRAW);CHECK(tap(10,440)==SSID_UI_REDRAW&&work.document.count==2);
    CHECK(tap(20,230)==SSID_UI_REDRAW);CHECK(tap(40,270)==SSID_UI_REDRAW&&!work.document.count);
}
static void import_save_close(const char *dir)
{
    reset();seed(1);cerberus_wifi_marauder_ssid_t document=work.document;
    CHECK(cerberus_ssid_list_session_prepare_document(&document)==ESP_OK);ssid_list_ui_import(&view);paint();
    CHECK(view.view==SSID_UI_IMPORT);preview(dir,"import");CHECK(tap(170,360)==SSID_UI_REDRAW&&work.document.count==2);
    CHECK(cerberus_ssid_list_session_prepare_document(&document)==ESP_OK);ssid_list_ui_import(&view);paint();
    CHECK(tap(120,440)==SSID_UI_REDRAW&&view.append_unique);CHECK(tap(170,360)==SSID_UI_REDRAW&&work.document.count==2);
    CHECK(cerberus_ssid_list_session_prepare_document(&document)==ESP_OK);ssid_list_ui_import(&view);paint();
    CHECK(tap(20,360)==SSID_UI_REDRAW&&view.view==SSID_UI_CONFIRM);uint64_t old=view.confirm_preview_id;
    CHECK(cerberus_ssid_list_session_prepare_document(&document)==ESP_OK&&io.preview_id!=old);paint();
    CHECK(tap(40,270)==SSID_UI_REDRAW&&work.document.count==2&&view.error==ESP_ERR_INVALID_STATE);
    CHECK(tap(10,440)==SSID_UI_REDRAW);CHECK(tap(20,360)==SSID_UI_REDRAW);CHECK(tap(40,270)==SSID_UI_REDRAW&&work.document.count==1);
    CHECK(tap(120,360)==SSID_UI_OPEN_FILES);paint();hold=true;CHECK(tap(220,360)==SSID_UI_REDRAW&&io.busy&&saves);
    uint64_t snapshot=io.saved_revision;CHECK(cerberus_ssid_list_add(&work,(const uint8_t*)"new",3)==CERBERUS_SSID_LIST_OK);paint();
    hold=false;CHECK(ssid_list_ui_tick(&view,true,false)==SSID_UI_REDRAW&&work.dirty&&work.revision!=snapshot);paint();
    preview(dir,"saved-newer-edits");hold=true;CHECK(tap(220,360)==SSID_UI_REDRAW&&io.busy);
    CHECK(tap(10,440)==SSID_UI_REDRAW&&view.closing&&closes);preview(dir,"closing");
    CHECK(strstr(drawn,"WAITING FOR SD WORKER")&&!strstr(drawn,"SAVE COPY"));
    CHECK(ssid_list_ui_tick(&view,true,false)==SSID_UI_NONE);poll_error=true;hold=false;
    CHECK(ssid_list_ui_tick(&view,true,false)==SSID_UI_NONE);poll_error=false;
    CHECK(ssid_list_ui_tick(&view,true,false)==SSID_UI_EXIT&&work.document.count==2&&work.dirty);
    ssid_list_ui_init(&view);paint();CHECK(work.document.count==2&&!view.closing);
    CHECK(ssid_list_ui_tick(&view,false,false)==SSID_UI_EXIT);ssid_list_ui_init(&view);paint();
    CHECK(ssid_list_ui_tick(&view,true,true)==SSID_UI_EXIT);
}
static void stale_privacy_failures(const char *dir)
{
    reset();seed(2);uint64_t revision=work.revision;
    CHECK(cerberus_ssid_list_remove(&work,0)==CERBERUS_SSID_LIST_OK);
    CHECK(tap(60,120)==SSID_UI_REDRAW&&view.view==SSID_UI_LIST&&work.revision==revision+1);
    CHECK(tap(60,120)==SSID_UI_REDRAW);CHECK(cerberus_ssid_list_clear(&work)==CERBERUS_SSID_LIST_OK);paint();
    CHECK(view.view==SSID_UI_LIST);seed(1);privacy=true;paint();preview(dir,"privacy");
    CHECK(strstr(drawn,"[NAME HIDDEN]")&&!strstr(drawn,"Lab name"));
    CHECK(tap(60,120)==SSID_UI_REDRAW);CHECK(tap(170,270)==SSID_UI_REDRAW&&view.view==SSID_UI_DETAIL);
    CHECK(strstr(view.message,"PRIVACY"));privacy=false;paint();CHECK(tap(170,270)==SSID_UI_REDRAW);
    privacy=true;CHECK(ssid_list_ui_tick(&view,true,false)==SSID_UI_REDRAW&&view.view==SSID_UI_DETAIL);paint();
    privacy=false;paint();CHECK(tap(10,440)==SSID_UI_REDRAW);
    calls=0;fail_after=5;CHECK(ssid_list_ui_draw(&view)==ESP_FAIL&&!view.painted_valid);fail_after=0;
    unsigned before=edits;CHECK(ssid_list_ui_tap(&view,20,120)==SSID_UI_NONE&&edits==before);paint();
    CHECK(tap(7,120)==SSID_UI_NONE&&tap(312,120)==SSID_UI_NONE&&tap(20,152)==SSID_UI_NONE);
    CHECK(tap(104,360)==SSID_UI_NONE&&tap(110,440)==SSID_UI_NONE&&tap(312,469)==SSID_UI_NONE);
    CHECK(tap(60,120)==SSID_UI_REDRAW);CHECK(tap(170,330)==SSID_UI_REDRAW);CHECK(tap(40,270)==SSID_UI_REDRAW&&!work.document.count);
    io.state=CERBERUS_SSID_SESSION_ERROR;io.error=ESP_FAIL;paint();preview(dir,"io-error");
}
static void stale_editor(void)
{
    reset();seed(1);CHECK(tap(60,120)==SSID_UI_REDRAW);CHECK(tap(20,270)==SSID_UI_REDRAW);
    uint64_t draft_revision=view.edit_revision;
    CHECK(cerberus_ssid_list_add(&work,(const uint8_t*)"elsewhere",9)==CERBERUS_SSID_LIST_OK);
    CHECK(tap(240,360)==SSID_UI_REDRAW&&view.view==SSID_UI_TEXT);
    CHECK(view.edit_revision==draft_revision&&view.painted_revision==work.revision);
    CHECK(tap(240,360)==SSID_UI_REDRAW&&view.error==ESP_ERR_INVALID_STATE&&work.document.count==2);
    CHECK(strstr(drawn,"LIST CHANGED OR BUSY"));
    CHECK(tap(10,440)==SSID_UI_REDRAW&&view.view==SSID_UI_DETAIL);
    CHECK(tap(170,330)==SSID_UI_REDRAW&&view.view==SSID_UI_CONFIRM);
    CHECK(cerberus_ssid_list_remove(&work,0)==CERBERUS_SSID_LIST_OK);paint();
    CHECK(tap(40,270)==SSID_UI_REDRAW&&view.error==ESP_ERR_INVALID_STATE&&work.document.count==1);
    CHECK(tap(10,440)==SSID_UI_REDRAW&&view.view==SSID_UI_LIST);
    cerberus_wifi_marauder_ssid_t document=work.document;
    CHECK(cerberus_ssid_list_session_prepare_document(&document)==ESP_OK);ssid_list_ui_import(&view);paint();
    CHECK(view.view==SSID_UI_IMPORT);cerberus_ssid_list_session_discard_preview();paint();
    CHECK(view.view==SSID_UI_LIST&&!view.preview_id&&!view.preview.count&&work.document.count==1);
    CHECK(strstr(drawn,"IMPORT NO LONGER AVAILABLE"));
    CHECK(cerberus_ssid_list_session_prepare_document(&document)==ESP_OK);ssid_list_ui_import(&view);paint();
    CHECK(tap(20,360)==SSID_UI_REDRAW&&view.view==SSID_UI_CONFIRM);
    cerberus_ssid_list_session_discard_preview();paint();
    CHECK(view.view==SSID_UI_LIST&&!view.preview_id&&!view.preview.count&&work.document.count==1);
}
static void tools_route(const char *dir)
{
    cerberus_ui_widget_theme_t theme={.background=UI_BACKGROUND,.panel=UI_PANEL,.text=UI_TEXT,.muted=UI_MUTED};
    calls=drawn_size=0;drawn[0]=0;CHECK(cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND)==ESP_OK);
    CHECK(sentinel_tools_draw(SENTINEL_TOOLS_EXTRAS,&theme)==ESP_OK);preview(dir,"tools-extras");
    CHECK(sentinel_tools_hit_test(SENTINEL_TOOLS_EXTRAS,12,70)==SENTINEL_TOOLS_SSID_LISTS);
    CHECK(sentinel_tools_hit_test(SENTINEL_TOOLS_EXTRAS,307,117)==SENTINEL_TOOLS_SSID_LISTS);
    CHECK(sentinel_tools_hit_test(SENTINEL_TOOLS_EXTRAS,12,124)==SENTINEL_TOOLS_QR);
    CHECK(sentinel_tools_hit_test(SENTINEL_TOOLS_EXTRAS,12,178)==SENTINEL_TOOLS_SCRIPTS);
    CHECK(sentinel_tools_hit_test(SENTINEL_TOOLS_EXTRAS,12,420)==SENTINEL_TOOLS_BACK);
    CHECK(sentinel_tools_hit_test(SENTINEL_TOOLS_EXTRAS,11,70)==SENTINEL_TOOLS_NONE);
    CHECK(sentinel_tools_hit_test(SENTINEL_TOOLS_EXTRAS,308,70)==SENTINEL_TOOLS_NONE);
}
int main(int argc,char **argv)
{
    CHECK(argc==2);editing(argv[1]);list_controls(argv[1]);import_save_close(argv[1]);
    stale_privacy_failures(argv[1]);stale_editor();tools_route(argv[1]);
    printf("SSID UI: %u checks; view %zu bytes; production keyboard and Tools routes passed\n",checks,sizeof(view));return 0;
}
