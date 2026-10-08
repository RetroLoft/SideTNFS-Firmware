/* Shared declarations for the generated network ownership adapter. */
int r11_radio_begin(const sidetnfs_system_settings_t *,char **);
void r11_radio_poll(void);
int r11_radio_link(void);
void r11_radio_leave(void);

void r13d_radio_diag(const char *where);
