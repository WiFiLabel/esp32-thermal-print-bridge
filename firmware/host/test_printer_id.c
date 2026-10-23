#include "printer_id.h"

#include <stdio.h>
#include <string.h>

static int g_failed;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_failed++;
    }
}

static void load(usb_printer_identity_t *id, const char *text)
{
    memset(id, 0, sizeof(*id));
    snprintf(id->device_id, sizeof(id->device_id), "%s", text);
    printer_id_parse(id);
}

int main(void)
{
    usb_printer_identity_t id;
    bool real = true;
    char fw[32];

    load(&id, "MFG:Zebra Technologies;CMD:ZPL,EPL;MDL:GK420d;");
    expect(strcmp(id.mfg, "Zebra Technologies") == 0, "mfg");
    expect(strcmp(id.mdl, "GK420d") == 0, "mdl");
    expect(strcmp(id.cmd, "ZPL,EPL") == 0, "cmd");
    expect(id.zpl, "zpl token");
    expect(id.present, "present");
    expect(strcmp(printer_id_name_base(&id, &real), "GK420d") == 0 && real, "name from model");

    load(&id, "manufacturer: Zebra ; model: ZD420 ; command set: zpl ");
    expect(strcmp(id.mfg, "Zebra") == 0, "manufacturer alias");
    expect(strcmp(id.mdl, "ZD420") == 0, "model alias");
    expect(id.zpl, "lowercase zpl");

    load(&id, "MFG:Godex;MDL:G500;CMD:EZPL;");
    expect(!id.zpl, "EZPL is not ZPL");
    expect(strcmp(printer_id_name_base(&id, &real), "G500") == 0 && real, "ezpl still names the model");

    load(&id, "CMD:ZPLII;");
    expect(id.zpl, "ZPLII starts with ZPL");

    load(&id, "MFG:Acme;CMD:EPL;");
    expect(!id.zpl, "epl only");
    expect(strcmp(printer_id_name_base(&id, &real), "Acme") == 0 && real, "name from manufacturer");

    {
        char name[64];
        printer_id_format_name(name, sizeof(name), "WiFiLabel-B448", "ZTC ZP 450-200dpi");
        expect(strcmp(name, "WiFiLabel-B448 ZTC ZP 450-200dpi") == 0, "brand plus model");
        printer_id_format_name(name, sizeof(name), "WiFiLabel-B448", NULL);
        expect(strcmp(name, "WiFiLabel-B448") == 0, "brand alone");
        char long_model[80];
        memset(long_model, 'M', 70);
        long_model[70] = '\0';
        printer_id_format_name(name, sizeof(name), "WiFiLabel-B448", long_model);
        expect(strlen(name) == 63, "instance stays within 63");
        expect(strncmp(name, "WiFiLabel-B448 ", 15) == 0, "long model keeps the brand");
    }

    memset(&id, 0, sizeof(id));
    expect(strcmp(printer_id_name_base(&id, &real), "WiFiLabel") == 0 && !real, "fallback name");
    expect(strcmp(printer_id_name_base(NULL, &real), "WiFiLabel") == 0 && !real, "null identity");

    {
        char raw[] = "MFG:Acme;MDL:Caf\xff" "e;CMD:ZPL;";
        memset(&id, 0, sizeof(id));
        memcpy(id.device_id, raw, sizeof(raw));
        printer_id_parse(&id);
        expect(strcmp(id.mfg, "Acme") == 0, "mfg survives a non-ascii model");
        expect(strcmp(id.mdl, "Caf?e") == 0, "non-ascii becomes ?");
        expect(id.zpl, "cmd survives a non-ascii model");
        expect(strchr(id.device_id, ';') != NULL, "separators stay");
    }

    {
        static const uint8_t hi[] = "\x02ZP 450-200dpi-200dpi,ZSP-002282AR,8,2104KB\x03";
        printer_id_take_firmware(hi, (int)sizeof(hi) - 1, fw, sizeof(fw));
        expect(strcmp(fw, "ZSP-002282AR") == 0, "hi firmware field");
    }

    {
        static const uint8_t bare[] = "\x02only-model\x03";
        printer_id_take_firmware(bare, (int)sizeof(bare) - 1, fw, sizeof(fw));
        expect(fw[0] == '\0', "hi without firmware");
    }

    {
        static const uint8_t bad[] = "\x02model,bad\xff" "fw,8\x03";
        printer_id_take_firmware(bad, (int)sizeof(bad) - 1, fw, sizeof(fw));
        expect(fw[0] == '\0', "non-printable firmware rejected");
    }

    printer_id_scrub(NULL);

    if (g_failed) {
        fprintf(stderr, "%d failed\n", g_failed);
        return 1;
    }
    printf("ok\n");
    return 0;
}
