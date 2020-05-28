#include <gccore.h>
#include <stdio.h>
#include <string.h>
#include <ogc/lwp_watchdog.h>
#include <assert.h>

#include "gba_mb_rom_bin.h"

//from my tests 50us seems to be the lowest
//safe si transfer delay in between calls
#define SI_TRANS_DELAY 50

ATTRIBUTE_ALIGN(32) static u8 recvbuf[32];
ATTRIBUTE_ALIGN(32) static u8 sendbuf[32];

// Initializes text mode rendering
static void initialize_video(void)
{
    void *xfb = NULL;
    GXRModeObj *rmode = NULL;
    VIDEO_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode&VI_NON_INTERLACE)
        VIDEO_WaitVSync();
    int x = 24, y = 32, w, h;
    w = rmode->fbWidth - (32);
    h = rmode->xfbHeight - (48);
    CON_InitEx(rmode, x, y, w, h);
    VIDEO_ClearFrameBuffer(rmode, xfb, COLOR_BLACK);
}

static volatile u32 transferComplete = 0;

static void transfer_callback(s32 chan, u32 ret)
{
    transferComplete = 1;
}

// Returns 1 if succeeded, 0 if failed
static int do_transfer(size_t sendBufLength, size_t recvBufLength)
{
    transferComplete = 0;
    SI_Transfer(
        1,
        sendbuf, sendBufLength,
        recvbuf, recvBufLength,
        transfer_callback,
        SI_TRANS_DELAY);
    u64 startTime = SYS_Time();
    while (transferComplete == 0)
    {
        if (ticks_to_secs(SYS_Time() - startTime) > 5)
            return 0;  // timed out
    }
    return 1;
}

static void link_doreset(void)
{
    sendbuf[0] = 0xFF;  // reset
    if (!do_transfer(1, 3))
        puts("link_doreset() timed out");
}

static void link_getstatus(void)
{
    memset(recvbuf, 0, sizeof(recvbuf));
    sendbuf[0] = 0x00;  // status
    if (!do_transfer(1, 3))
        puts("link_getstatus() timed out");
}

// Receives a 32-bit message
static u32 link_recv(void)
{
    memset(recvbuf, 0, 32);
    sendbuf[0] = 0x14;  // read
    if (!do_transfer(1, 5))
        puts("link_recv() timed out");
    return (recvbuf[0] <<  0)
         | (recvbuf[1] <<  8)
         | (recvbuf[2] << 16)
         | (recvbuf[3] << 24);
}

// Sends a 32-bit message
static void link_send(u32 msg)
{
    sendbuf[0]= 0x15;
    sendbuf[1]= (msg >>  0) & 0xFF;
    sendbuf[2]= (msg >>  8) & 0xFF;
    sendbuf[3]= (msg >> 16) & 0xFF;
    sendbuf[4]= (msg >> 24) & 0xFF;
    if (!do_transfer(5, 1))
        puts("link_send() timed out");
}

// prints a string at the specified coordinates on the screen
static void print_at(unsigned int row, unsigned int col, const char *text)
{
    printf("\x1B[s\x1B[%u;%uH%s\x1B[u", row, col, text);
}

enum
{
    STATUS_NO_RESPONSE = 0,  // nothing connected
    STATUS_WRONG_DEVICE,     // something other than a GBA is connected
    STATUS_CONNECTED,        // GBA is connected
};

static int connectionTimeout = 0;

static int poll_gba_status(void)
{
    static int status;

    u32 probeStatus = SI_Probe(1);
    u8 error = (probeStatus & 0xFF);  // lower byte contains error information
    u32 type = probeStatus & ~0xFFFF;  // upper two bytes contain the type of peripheral

    if (error != 0)
    {
        switch (error)
        {
        case 0x80:  // busy
            break;
        case 0x08:  // no response
            break;
        }
        if (connectionTimeout > 0)
            connectionTimeout--;
        if (connectionTimeout == 0)
            status = STATUS_NO_RESPONSE;
    }
    else
    {
        if (type == 0x00040000)  // GBA
        {
            status = STATUS_CONNECTED;
            connectionTimeout = 60;
        }
        else if (type == 0x08000000)  // This sometimes gets detected when a GBA is connected. Ignore it for now. 
        {
        }
        else
        {
            status = STATUS_WRONG_DEVICE;
            connectionTimeout = 60;
        }
    }

    // DEBUG: print probe value
    char buf[13];
    sprintf(buf, "0x%08X  ", probeStatus);
    print_at(1, 46, buf);

    // DEBUG: print GBA connection status
    char *statusText = NULL;
    switch (status)
    {
    case STATUS_NO_RESPONSE:  statusText = "No response "; break;
    case STATUS_WRONG_DEVICE: statusText = "Wrong Device"; break;
    case STATUS_CONNECTED:    statusText = "Connected!  "; break;
    }
    print_at(2, 46, statusText);

    return status;
}

// Draws a spinning throbber each frame to see if the Wii is frozen or not
static void draw_throbber(void)
{
    static unsigned int frame;
    char str[2] = {0};

    str[0] = "|/-\\"[frame % 4];
    print_at(0, 60, str);
    frame++;
}

static u32 calc_key(u32 size)
{
    u32 ret = 0;
    size = (size - 0x200) >> 3;
    int res1 = (size & 0x3F80) << 1;
    res1 |= (size & 0x4000) << 2;
    res1 |= (size & 0x7F);
    res1 |= 0x380000;
    int res2 = res1;
    res1 = res2 >> 0x10;
    int res3 = res2 >> 8;
    res3 += res1;
    res3 += res2;
    res3 <<= 24;
    res3 |= res2;
    res3 |= 0x80808080;

    if ((res3 & 0x200) == 0)
    {
        ret |= (((res3 >>  0) & 0xFF) ^ 0x4B) << 24;
        ret |= (((res3 >>  8) & 0xFF) ^ 0x61) << 16;
        ret |= (((res3 >> 16) & 0xFF) ^ 0x77) <<  8;
        ret |= (((res3 >> 24) & 0xFF) ^ 0x61) <<  0;
    }
    else
    {
        ret |= (((res3 >>  0) & 0xFF) ^ 0x73) << 24;
        ret |= (((res3 >>  8) & 0xFF) ^ 0x65) << 16;
        ret |= (((res3 >> 16) & 0xFF) ^ 0x64) <<  8;
        ret |= (((res3 >> 24) & 0xFF) ^ 0x6F) <<  0;
    }
    return ret;
}

static unsigned int docrc(u32 crc, u32 val)
{
    int i;
    for (i = 0; i < 0x20; i++)
    {
        if ((crc^val)&1)
        {
            crc >>= 1;
            crc ^= 0xa1c1;
        }
        else
            crc >>= 1;
        val >>= 1;
    }
    return crc;
}

#define GBA_HEADER_SIZE 0xC0

static void transfer_program(const u8 *program, size_t size)
{
    u32 sendSize = (size + 7) & ~7;

    assert(size == ((size + 3) & ~3));  // size must be a multiple of 4

    puts("Waiting for BIOS");
    recvbuf[2] = 0;
    while(!(recvbuf[2]&0x10))
    {
        link_doreset();
        link_getstatus();
    }
    puts("BIOS ready");

    u32 localKey = calc_key(sendSize);
    u32 remoteKey = __builtin_bswap32(__builtin_bswap32(link_recv()) ^ 0x7365646F);
    int offset = 0;

    printf("Local key = 0x%08X\nRemote key = 0x%08X\n", localKey, remoteKey);

    // Send our key
    link_send(__builtin_bswap32(localKey));

    fputs("Sending GBA header \x1B[s", stdout);
    while (offset < GBA_HEADER_SIZE)
    {
        u32 msg = (program[offset + 3] << 24)
                | (program[offset + 2] << 16)
                | (program[offset + 1] <<  8)
                | (program[offset + 0] <<  0);
        link_send(msg);
        offset += 4;

        printf("\x1B[u (%u/%u bytes) ", offset, GBA_HEADER_SIZE);
    }
    puts("Done!");

    fputs("Sending ROM \x1B[s", stdout);
    u32 fcrc = 0x15A0;
    while (offset < sendSize)
    { 
        u32 msg;

        if (offset >= size)
            msg = 0;  // pad with zeros
        else
            msg = (program[offset + 3] << 24)
                | (program[offset + 2] << 16)
                | (program[offset + 1] <<  8)
                | (program[offset + 0] <<  0);

        fcrc = docrc(fcrc, msg);

        // encrypt message
        remoteKey = (remoteKey * 0x6177614B) + 1;
        msg ^= remoteKey;
        msg ^= ~(offset + (0x20 << 20)) + 1;
        msg ^= 0x20796220;
        link_send(msg);
        offset += 4;

        printf("\x1B[u (%u/%u bytes) ", offset - GBA_HEADER_SIZE, sendSize - GBA_HEADER_SIZE);
    }
    puts("Done!");

    fcrc |= (size << 16);
    //printf("ROM done! CRC: %08x\n", fcrc);
    //send over CRC
    remoteKey = (remoteKey * 0x6177614B) + 1;
    fcrc ^= remoteKey;
    fcrc ^= (~(offset + (0x20 << 20))) + 1;
    fcrc ^= 0x20796220;
    link_send(fcrc);
    //get crc back (unused)
    link_recv();
}

static void print_connect_gba_text(void)
{
    puts("Please connect a GBA into port 2 and turn\n"
         "the power on while holding the Start and\n"
         "Select buttons."); 
}

int main(int argc, char **argv)
{
    u32 sendVal = 0;  // value to send to GBA

    enum
    {
        STATE_WAIT_GBA,
        STATE_GBA_READY,
        STATE_IDLE,
    } state;

    initialize_video();
    PAD_Init();

    u16 prevKeys = 0;  // keys that were down last frame
    u16 keys = 0;      // keys that are down this frame
    u16 pressedKeys = 0;  // keys that were pressed this frame
    u16 releasedKeys = 0;  // keys that were released this frame

    state = STATE_WAIT_GBA;
    print_connect_gba_text();

    while (1)
    {
        // Read controller inputs
        PADStatus padStatus[PAD_CHANMAX];
        PAD_Read(padStatus);
        if (padStatus[0].err == PAD_ERR_NONE)
        {
            keys = padStatus[0].button;
            pressedKeys = keys & (keys ^ prevKeys);
            releasedKeys = ~keys & (keys ^ prevKeys);
            prevKeys = keys;
        }
        else
        {
            // reset keys
            prevKeys = keys = pressedKeys = releasedKeys = 0;
        }

        // Exit any time if START was pressed
        if (pressedKeys & PAD_BUTTON_START)
            break;

        int gbaStatus = poll_gba_status();

        switch (state)
        {
        case STATE_WAIT_GBA:
            if (gbaStatus == STATUS_CONNECTED)
            {
                // Wait for BIOS to become ready
                link_doreset();
                link_getstatus();
                if (recvbuf[2] & 0x10)
                {
                    puts("GBA Ready! Press A to transfer program.");
                    state = STATE_GBA_READY;
                }
            }
            break;
        case STATE_GBA_READY:
            if (gbaStatus != STATUS_CONNECTED)
            {
                puts("Lost connection with GBA.");

                state = STATE_WAIT_GBA;
                print_connect_gba_text();
                break;
            }
            if (pressedKeys & PAD_BUTTON_A)
            {
                puts("Transferring program");
                transfer_program(gba_mb_rom_bin, gba_mb_rom_bin_size);
                state = STATE_IDLE;
                puts("Program transfer done!");
            }
            break;
        case STATE_IDLE:
            if (gbaStatus != STATUS_CONNECTED)
            {
                puts("Lost connection with GBA.");

                state = STATE_WAIT_GBA;
                print_connect_gba_text();
                break;
            }

            // recv from GBA
            u32 val = link_recv();
            if (recvbuf[4] & 8)
                printf("<<< recv 0x%08X\n", val);

            if (pressedKeys & PAD_BUTTON_UP)
                sendVal++;
            else if (pressedKeys & PAD_BUTTON_DOWN)
                sendVal--;
            else if (pressedKeys & PAD_BUTTON_A)  // Send value to GBA
            {
                link_send(sendVal);
                printf(">>> send 0x%08X\n", sendVal);
            }
            else if (pressedKeys & PAD_TRIGGER_Z)  // Disconnect GBA
            {
                printf("disconnecting GBA");
                connectionTimeout = 0;

                state = STATE_WAIT_GBA;
                print_connect_gba_text();
            }
            
            break;
        }

        draw_throbber();
        VIDEO_WaitVSync();
    }
    puts("exiting");

    VIDEO_WaitVSync();
    return 0;
}
