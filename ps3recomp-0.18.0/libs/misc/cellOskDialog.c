/*
 * ps3recomp - cellOskDialog HLE implementation
 *
 * Provides an on-screen keyboard stub.  When a game requests text input,
 * the prompt is logged and an empty string (or configurable default) is
 * returned immediately.  In a full implementation this would show an
 * actual input dialog.
 */

#include "cellOskDialog.h"
#include <stdio.h>
#include <string.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../guest_struct.h"   /* GUEST_EA, vm_read/vm_write: guest EA -> host */
#include "../system/cellSysutil.h"   /* cellSysutilQueueEvent: the dialog's LOADED/FINISHED events */
#include <stdlib.h>

/* The sysutil events the real OSK raises, in the order a title sees them. */
/* Numbering checked against .hack//Versus's own sysutil callback (func_0038C264
 * compares 0x502..0x508): a first version sent them one lower, so the title saw
 * LOADED and UNLOADED but never FINISHED and waited for the name forever. */
#define CELL_SYSUTIL_OSKDIALOG_LOADED         0x0502
#define CELL_SYSUTIL_OSKDIALOG_FINISHED       0x0503
#define CELL_SYSUTIL_OSKDIALOG_UNLOADED       0x0504
#define CELL_SYSUTIL_OSKDIALOG_INPUT_ENTERED  0x0505

/* ---------------------------------------------------------------------------
 * Internal state
 *
 * There is no cellOskDialogInit/End in the SDK: a title loads the dialog
 * straight away (.hack//Versus prepares it with the Set... and Ext... calls
 * and then cellOskDialogLoadAsync). Requiring an Init nobody calls made LoadAsync fail
 * with NOT_INITIALIZED, silently, and the hub that asks the player's name
 * never advanced. s_osk_initialized is kept only for the (non-SDK) Init/End
 * pair; nothing else tests it.
 * -----------------------------------------------------------------------*/

static int  s_osk_initialized = 0;
static int  s_osk_loaded = 0;
static char s_default_response[CELL_OSK_MAX_TEXT_LENGTH + 1] = "";

/* Stored input result (UTF-16) */
static CellOskDialogChar s_result_text[CELL_OSK_MAX_TEXT_LENGTH + 1];
static u32               s_result_length = 0;

/* Convert narrow string to UTF-16LE (simple ASCII subset) */
static void osk_ascii_to_utf16(const char* src, CellOskDialogChar* dst,
                                u32 maxChars)
{
    u32 i;
    for (i = 0; i < maxChars && src[i] != '\0'; i++)
        dst[i] = (CellOskDialogChar)src[i];
    dst[i] = 0;
}

/* Convert UTF-16LE to narrow string for logging (ASCII subset) */
static void osk_utf16_to_ascii(const CellOskDialogChar* src, char* dst,
                                u32 maxChars)
{
    u32 i;
    for (i = 0; i < maxChars && src[i] != 0; i++)
        dst[i] = (src[i] < 128) ? (char)src[i] : '?';
    dst[i] = '\0';
}

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * Guest struct access
 *
 * CellOskDialogInputFieldInfo is { CellOskDialogChar* message;
 * CellOskDialogChar* init_text; u32 limit_length; } and
 * CellOskDialogCallbackReturnParam is { s32 result; u32 numCharsResultString;
 * CellOskDialogChar* pResultString; }. Both lead with (or contain) pointers,
 * which the guest stores in four bytes and we in eight, so a cast misplaces
 * every field after the first. The strings are UTF-16 and big-endian, so the
 * code units come across one at a time too.
 * -----------------------------------------------------------------------*/
enum {
    OSKIF_O_MESSAGE = 0, OSKIF_O_INIT_TEXT = 4, OSKIF_O_LIMIT_LENGTH = 8,
    OSKRET_O_RESULT = 0, OSKRET_O_NUMCHARS = 4, OSKRET_O_PRESULTSTRING = 8
};

/* Copy a NUL-terminated big-endian UTF-16 string out of guest memory. */
static u32 osk_guest_utf16(u32 ea, CellOskDialogChar* dst, u32 max)
{
    u32 n = 0;
    if (!ea)
        return 0;
    while (n < max) {
        u16 c = vm_read16(ea + n * 2);
        if (!c)
            break;
        dst[n++] = c;
    }
    dst[n] = 0;
    return n;
}

void cellOskDialogSetDefaultResponse(const char* text)
{
    if (text) {
        strncpy(s_default_response, GUEST_PTR(text, const char*),
                CELL_OSK_MAX_TEXT_LENGTH);
        s_default_response[CELL_OSK_MAX_TEXT_LENGTH] = '\0';
    }
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellOskDialogInit(u32 container)
{
    (void)container;
    printf("[cellOskDialog] Init()\n");

    if (s_osk_initialized)
        return CELL_OSK_ERROR_ALREADY_INITIALIZED;

    s_osk_initialized = 1;
    s_osk_loaded = 0;
    return CELL_OK;
}

s32 cellOskDialogEnd(void)
{
    printf("[cellOskDialog] End()\n");

    if (!s_osk_initialized)
        return CELL_OSK_ERROR_NOT_INITIALIZED;

    s_osk_initialized = 0;
    s_osk_loaded = 0;
    return CELL_OK;
}

s32 cellOskDialogLoadAsync(u32 container,
                           const CellOskDialogParam* dialogParam,
                           const CellOskDialogInputFieldInfo* inputFieldInfo)
{
    (void)container; (void)dialogParam;

    if (s_osk_loaded)
        return CELL_OSK_ERROR_DIALOG_ALREADY_LOADED;

    s_osk_loaded = 1;
    /* PS3_OSK_TEXT=<text>: what the invisible keyboard "types" when the title
     * gives no initial text (a player name, typically). */
    if (s_default_response[0] == '\0') {
        const char* e = getenv("PS3_OSK_TEXT");
        strncpy(s_default_response, (e && e[0]) ? e : "PLAYER", CELL_OSK_MAX_TEXT_LENGTH);
        s_default_response[CELL_OSK_MAX_TEXT_LENGTH] = '\0';
    }

    u32 info_ea    = GUEST_EA(inputFieldInfo);
    u32 message_ea = info_ea ? vm_read32(info_ea + OSKIF_O_MESSAGE)   : 0;
    u32 init_ea    = info_ea ? vm_read32(info_ea + OSKIF_O_INIT_TEXT) : 0;

    static CellOskDialogChar host_init[CELL_OSK_MAX_TEXT_LENGTH + 1];
    u32 init_len = osk_guest_utf16(init_ea, host_init, CELL_OSK_MAX_TEXT_LENGTH);

    /* Log the prompt if available */
    if (message_ea) {
        static CellOskDialogChar host_msg[CELL_OSK_MAX_TEXT_LENGTH + 1];
        osk_guest_utf16(message_ea, host_msg, CELL_OSK_MAX_TEXT_LENGTH);
        char prompt[256];
        osk_utf16_to_ascii(host_msg, prompt, sizeof(prompt) - 1);
        printf("[cellOskDialog] LoadAsync - Prompt: \"%s\"\n", prompt);
    } else {
        printf("[cellOskDialog] LoadAsync - (no prompt)\n");
    }

    if (init_ea) {
        char init[256];
        osk_utf16_to_ascii(host_init, init, sizeof(init) - 1);
        printf("[cellOskDialog] Initial text: \"%s\"\n", init);
    }

    /* Prepare the result: the title's initial text if it gave one (an existing
     * name being edited), else the default response. */
    memset(s_result_text, 0, sizeof(s_result_text));
    if (init_len > 0) {
        memcpy(s_result_text, host_init,
               (init_len + 1) * sizeof(CellOskDialogChar));
        s_result_length = init_len;
    } else if (s_default_response[0] != '\0') {
        osk_ascii_to_utf16(s_default_response, s_result_text,
                           CELL_OSK_MAX_TEXT_LENGTH);
        s_result_length = (u32)strlen(s_default_response);
        printf("[cellOskDialog] Returning default response: \"%s\"\n",
               s_default_response);
    } else {
        s_result_length = 0;
    }

    /* The dialog "opens", the player "types" and confirms, and it "closes":
     * the events a title waits for before it calls cellOskDialogUnloadAsync
     * to read the text. Every registered slot gets them, as sysutil does. */
    for (int slot = 0; slot < CELL_SYSUTIL_MAX_CALLBACKS; ++slot) {
        cellSysutilQueueEvent(slot, CELL_SYSUTIL_OSKDIALOG_LOADED, 0);
        cellSysutilQueueEvent(slot, CELL_SYSUTIL_OSKDIALOG_INPUT_ENTERED, 0);
        cellSysutilQueueEvent(slot, CELL_SYSUTIL_OSKDIALOG_FINISHED, 0);
    }
    return CELL_OK;
}

/* Guest CellOskDialogCallbackReturnParam: result, numCharsResultString (the
 * buffer's capacity on entry, the text's length on return), pResultString. */
static void osk_write_result(u32 res_ea, s32 result)
{
    u32 cap = vm_read32(res_ea + OSKRET_O_NUMCHARS);
    u32 n = s_result_length;
    if (cap && n > cap) n = cap;
    vm_write32(res_ea + OSKRET_O_RESULT, (u32)result);
    vm_write32(res_ea + OSKRET_O_NUMCHARS, n);
    u32 str_ea = vm_read32(res_ea + OSKRET_O_PRESULTSTRING);
    if (str_ea) {
        /* UTF-16, big-endian in guest memory: one unit at a time. */
        for (u32 i = 0; i < n; i++)
            vm_write16(str_ea + i * 2, s_result_text[i]);
        if (!cap || n < cap)
            vm_write16(str_ea + n * 2, 0);
    }
}

s32 cellOskDialogUnloadAsync(CellOskDialogCallbackReturnParam* result)
{
    if (!s_osk_loaded)
        return CELL_OSK_ERROR_DIALOG_NOT_LOADED;

    printf("[cellOskDialog] UnloadAsync()\n");

    if (result)
        osk_write_result(GUEST_EA(result), CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK);

    s_osk_loaded = 0;
    for (int slot = 0; slot < CELL_SYSUTIL_MAX_CALLBACKS; ++slot)
        cellSysutilQueueEvent(slot, CELL_SYSUTIL_OSKDIALOG_UNLOADED, 0);
    return CELL_OK;
}

s32 cellOskDialogGetSize(u32* width, u32* height)
{
    if (!width || !height)
        return CELL_OSK_ERROR_INVALID_PARAMETER;

    /* Standard PS3 OSK dimensions */
    vm_write32((u32)(uintptr_t)width, (u32)640);
    vm_write32((u32)(uintptr_t)height, (u32)240);
    return CELL_OK;
}

s32 cellOskDialogAbort(void)
{
    printf("[cellOskDialog] Abort()\n");

    if (!s_osk_loaded)
        return CELL_OSK_ERROR_DIALOG_NOT_LOADED;

    s_osk_loaded = 0;
    return CELL_OK;
}

s32 cellOskDialogSetInitialInputDevice(u32 device)
{
    (void)device;
    return CELL_OK;
}

s32 cellOskDialogSetInitialKeyLayout(u32 layout)
{
    (void)layout;
    printf("[cellOskDialog] SetInitialKeyLayout(%u)\n", layout);
    return CELL_OK;
}

s32 cellOskDialogSetLayoutMode(u32 mode)
{
    (void)mode;
    printf("[cellOskDialog] SetLayoutMode(%u)\n", mode);
    return CELL_OK;
}

s32 cellOskDialogSetSeparateWindowOption(u32 option)
{
    (void)option;
    return CELL_OK;
}

s32 cellOskDialogGetInputText(CellOskDialogCallbackReturnParam* result)
{
    if (!result)
        return CELL_OSK_ERROR_INVALID_PARAMETER;

    osk_write_result(GUEST_EA(result), (s_result_length > 0)
                     ? CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK
                     : CELL_OSK_DIALOG_INPUT_FIELD_RESULT_NO_INPUT_TEXT);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Options and the cellOskExtUtility module: all presentation (languages,
 * colours, scale, dimmer, pointer, dictionaries, hook callbacks) of a keyboard
 * that is never drawn. Each accepts its arguments and reports success; the
 * force-finish and hook callbacks are never invoked because the dialog
 * finishes on its own the moment it loads.
 * -----------------------------------------------------------------------*/
s32 cellOskDialogSetDeviceMask(u32 mask) { (void)mask; return CELL_OK; }
s32 cellOskDialogSetKeyLayoutOption(u32 option) { (void)option; return CELL_OK; }
s32 cellOskDialogAddSupportLanguage(u32 languages) { (void)languages; return CELL_OK; }
s32 cellOskDialogDisableDimmer(void) { return CELL_OK; }
s32 cellOskDialogExtSetInitialScale(u32 unused) { (void)unused; return CELL_OK; }   /* float arg, in an FPR */
s32 cellOskDialogExtSetBaseColor(u32 unused) { (void)unused; return CELL_OK; }      /* four floats, in FPRs */
s32 cellOskDialogExtRegisterForceFinishCallback(u32 opd) { (void)opd; return CELL_OK; }
s32 cellOskDialogExtRegisterConfirmWordFilterCallback(u32 opd) { (void)opd; return CELL_OK; }
s32 cellOskDialogExtRegisterKeyboardEventHookCallback(u32 flags, u32 opd) { (void)flags; (void)opd; return CELL_OK; }
s32 cellOskDialogExtRegisterKeyboardEventHookCallbackEx(u32 flags, u32 opd) { (void)flags; (void)opd; return CELL_OK; }
s32 cellOskDialogExtInputDeviceLock(void) { return CELL_OK; }
s32 cellOskDialogExtInputDeviceUnlock(void) { return CELL_OK; }
s32 cellOskDialogExtSetPointerEnable(u32 enable) { (void)enable; return CELL_OK; }
s32 cellOskDialogExtUpdatePointerDisplayPos(u32 pos) { (void)pos; return CELL_OK; }
s32 cellOskDialogExtEnableClipboard(void) { return CELL_OK; }
s32 cellOskDialogExtEnableHalfByteKana(void) { return CELL_OK; }
s32 cellOskDialogExtDisableHalfByteKana(void) { return CELL_OK; }
s32 cellOskDialogExtAddOptionDictionary(u32 dict) { (void)dict; return CELL_OK; }
s32 cellOskDialogExtAddJapaneseOptionDictionary(u32 dict) { (void)dict; return CELL_OK; }
s32 cellOskDialogExtUpdateInputText(void) { return CELL_OK; }
s32 cellOskDialogExtSendFinishMessage(u32 finish) { (void)finish; return CELL_OK; }
