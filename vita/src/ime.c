#include "ime.h"

#include <string.h>
#include <stdint.h>
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>

#define IME_MAX 2048

static uint16_t ime_title[SCE_IME_DIALOG_MAX_TITLE_LENGTH];
static uint16_t ime_initial[IME_MAX + 1];
static uint16_t ime_buffer[IME_MAX + 1];
static int ime_open_flag = 0;

// UTF-8 -> UTF-16 (BMP; lo que no cabe se cambia por '?')
static void utf8_to_utf16(const char *in, uint16_t *out, int cap) {
    const unsigned char *s = (const unsigned char *)in;
    int n = 0;
    while (*s && n < cap - 1) {
        uint32_t c;
        if (*s < 0x80) { c = *s++; }
        else if ((*s & 0xE0) == 0xC0 && s[1]) { c = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); s += 2; }
        else if ((*s & 0xF0) == 0xE0 && s[1] && s[2]) { c = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); s += 3; }
        else { c = '?'; s++; while ((*s & 0xC0) == 0x80) s++; }
        out[n++] = (uint16_t)c;
    }
    out[n] = 0;
}

// UTF-16 -> UTF-8 (incluye pares sustitutos, para emojis del teclado)
static void utf16_to_utf8(const uint16_t *in, char *out, int cap) {
    int n = 0;
    for (; *in; in++) {
        uint32_t c = *in;
        if (c >= 0xD800 && c <= 0xDBFF && in[1] >= 0xDC00 && in[1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (in[1] - 0xDC00);
            in++;
        }
        char tmp[4];
        int len;
        if (c < 0x80) { tmp[0] = (char)c; len = 1; }
        else if (c < 0x800) { tmp[0] = (char)(0xC0 | (c >> 6)); tmp[1] = (char)(0x80 | (c & 0x3F)); len = 2; }
        else if (c < 0x10000) { tmp[0] = (char)(0xE0 | (c >> 12)); tmp[1] = (char)(0x80 | ((c >> 6) & 0x3F)); tmp[2] = (char)(0x80 | (c & 0x3F)); len = 3; }
        else { tmp[0] = (char)(0xF0 | (c >> 18)); tmp[1] = (char)(0x80 | ((c >> 12) & 0x3F)); tmp[2] = (char)(0x80 | ((c >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (c & 0x3F)); len = 4; }
        if (n + len >= cap) break;
        memcpy(out + n, tmp, len);
        n += len;
    }
    out[n] = '\0';
}

int ime_open(const char *title, const char *initial, int kind, int max_len) {
    if (ime_open_flag) return -1;
    if (max_len <= 0 || max_len > IME_MAX) max_len = IME_MAX;

    utf8_to_utf16(title, ime_title, SCE_IME_DIALOG_MAX_TITLE_LENGTH);
    utf8_to_utf16(initial ? initial : "", ime_initial, max_len + 1);
    memset(ime_buffer, 0, sizeof(ime_buffer));

    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = SCE_IME_LANGUAGE_SPANISH | SCE_IME_LANGUAGE_ENGLISH;
    p.languagesForced = SCE_FALSE;
    p.type = kind == IME_KIND_URL ? SCE_IME_TYPE_URL
           : kind == IME_KIND_NUMBER ? SCE_IME_TYPE_EXTENDED_NUMBER
           : SCE_IME_TYPE_DEFAULT;
    p.option = kind == IME_KIND_TEXT ? 0 : SCE_IME_OPTION_NO_AUTO_CAPITALIZATION;
    p.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    p.title = ime_title;
    p.maxTextLength = (SceUInt32)max_len;
    p.initialText = ime_initial;
    p.inputTextBuffer = ime_buffer;

    int r = sceImeDialogInit(&p);
    if (r >= 0) ime_open_flag = 1;
    return r;
}

int ime_is_open(void) { return ime_open_flag; }

int ime_poll(char *out, int out_cap) {
    if (!ime_open_flag) return -1;
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return 0;

    SceImeDialogResult res;
    memset(&res, 0, sizeof(res));
    sceImeDialogGetResult(&res);
    sceImeDialogTerm();
    ime_open_flag = 0;

    if (res.button != SCE_IME_DIALOG_BUTTON_ENTER) return -1;
    utf16_to_utf8(ime_buffer, out, out_cap);
    return 1;
}
