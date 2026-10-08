/* Included in network.c, where ownership of cyw43_initialized lives.
 * No driver reinitialization or teardown in the cooperative SD loop. */
static int r13d_previous_raw=999;
static const char *r13d_raw_name(int raw){
    switch(raw){
    case CYW43_LINK_DOWN:return "down";
    case CYW43_LINK_JOIN:return "joining";
    case CYW43_LINK_NOIP:return "associated-no-ip";
    case CYW43_LINK_UP:return "up";
    case CYW43_LINK_FAIL:return "link-fail";
    case CYW43_LINK_NONET:return "network-not-found";
    case CYW43_LINK_BADAUTH:return "bad-auth";
    case -100:return "radio-uninitialized";
    default:return "unknown";
    }
}
void r13d_radio_diag(const char *where){
    int raw=cyw43_initialized?cyw43_tcpip_link_status(&cyw43_state,CYW43_ITF_STA):-100;
    /* Periodic diagnostics only report transitions; explicit failures always log. */
    if(raw!=r13d_previous_raw || (where[0]!='s' && where[0]!='p')){
        LOG(WIFI,DEBUG,"R13D radio where=%s raw=%d status=%s initialized=%u\r\n",where,raw,r13d_raw_name(raw),(unsigned)cyw43_initialized);
        r13d_previous_raw=raw;
    }
}
int r11_radio_begin(const sidetnfs_system_settings_t *settings,char **password){
    if(!cyw43_initialized)return -100;
    r13d_radio_diag("begin");
    int result=network_init_with_settings(true,NETWORK_CONNECTION_ASYNC,password,settings);
    LOG(WIFI,DEBUG,"R13D join enqueue_result=%d\r\n",result);
    return result;
}
void r11_radio_poll(void){network_safe_poll();}
int r11_radio_link(void){
    if(!cyw43_initialized)return -1;
    ConnectionStatus s=get_network_connection_status();
    r13d_radio_diag("poll");
    if(s==CONNECTED_WIFI_IP)return 1;
    if(s==GENERIC_ERROR || s==BADAUTH_ERROR || s==CONNECT_FAILED_ERROR)return -1;
    return 0;
}
void r11_radio_leave(void){
    if(!cyw43_initialized)return;
    r13d_radio_diag("leave");
    cyw43_arch_lwip_begin();
    cyw43_wifi_leave(&cyw43_state,CYW43_ITF_STA);
    cyw43_arch_lwip_end();
}
