#include "aime_reader_app.h"

#include "aime_host.h"
#include "pn532_reader.h"

void aime_reader_app_init(void)
{
    pn532_reader_init();
    aime_host_init();
}

void aime_reader_app_task(void)
{
    pn532_reader_task();
    aime_host_task();
}
