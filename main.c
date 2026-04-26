#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <libgen.h>

#include <getopt.h>
#include <limits.h>
#include <unistd.h>
#include <termios.h>
#include <pthread.h>

#include <signal.h>

#include "main.h"
#include "usf.h"
#include "audio.h"
#include "memory.h"

#define VERSION "0.260425.1"

static const struct
{
    char wildcard[12];
    char * replacement;
} wildcards[7] =
{
    {"%game%", game},
    {"%genre%", genre},
    {"%title%", title},
    {"%artist%", artist},
    {"%copyright%", copyright},
    {"%year%", year},
    {"", NULL} // terminal
};

char filename[PATH_MAX+1];

void StopEmulation(void)
{
    cpu_running = 0;
}

void DisplayError (char * Message, ...)
{
    char Msg[1000];
    va_list ap;

    va_start( ap, Message );
    vsprintf( Msg, Message, ap );
    va_end( ap );

    printf("Error: %s\n", Msg);
}

/* set by key thread to request going back to previous track */
static volatile bool go_prev = false;

/* set by key thread or second Ctrl-C to abort all playback */
static volatile bool quit_all = false;

/* set by -L flag */
static volatile bool loopPlaylist = false;

/* set by --forever flag or 'l' key; persists across tracks */
static volatile bool playForever = false;

/* natural track_time saved when entering forever mode, for toggling back */
static uint32_t saved_track_time = 0;

static pthread_mutex_t term_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile bool display_ready = false;

static void print_status(void)
{
    if (!isatty(STDOUT_FILENO)) return;
    uint32_t cur_ms = (uint32_t)play_time;
    if (track_time >> (sizeof(uint32_t)*8 - 1)) {
        printf("\r\033[K[n] next  [p] previous  [l] forever  [q] quit    %u:%02u / Forever",
            cur_ms / 60000, (cur_ms / 1000) % 60);
    } else {
        uint32_t tot_ms = track_time;
        printf("\r\033[K[n] next  [p] previous  [l] forever  [q] quit    %u:%02u / %u:%02u",
            cur_ms / 60000, (cur_ms / 1000) % 60,
            tot_ms / 60000, (tot_ms / 1000) % 60);
    }
    fflush(stdout);
}

static struct termios orig_termios;
static bool raw_mode_active = false;

static void disable_raw_mode(void)
{
    if (raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_mode_active = false;
    }
}

static void enable_raw_mode(void)
{
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_active = true;
}

static void *key_reader(void *arg)
{
    (void)arg;
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        switch (c) {
        case 'n': case 'N':
            StopEmulation();
            break;
        case 'p': case 'P':
            go_prev = true;
            StopEmulation();
            break;
        case 'l': case 'L':
            playForever = !playForever;
            if (playForever) {
                saved_track_time = track_time & 0x7FFFFFFF;
                track_time |= 1u << (sizeof(uint32_t)*8 - 1);
            } else {
                track_time = saved_track_time;
            }
            pthread_mutex_lock(&term_mutex);
            if (display_ready) print_status();
            pthread_mutex_unlock(&term_mutex);
            break;
        case 'q': case 'Q':
            quit_all = true;
            StopEmulation();
            break;
        }
    }
    return NULL;
}

static void *status_thread(void *arg)
{
    (void)arg;
    while (!quit_all) {
        pthread_mutex_lock(&term_mutex);
        if (display_ready)
            print_status();
        pthread_mutex_unlock(&term_mutex);
        usleep(250000);
    }
    return NULL;
}

void sig(int signo)
{
    switch(signo)
    {
    case SIGUSR1:
        play_time = track_time;
        break;
    case SIGINT:
        quit_all = true;
        StopEmulation();
        Release_Memory();
        DoneAudio();
        disable_raw_mode();
        exit(0);
    }
}

void InitSigHandler(void)
{
    sigset_t blockset;
    sigemptyset(&blockset);
    sigaddset(&blockset, SIGUSR1);
    sigaddset(&blockset, SIGINT);
    sigprocmask(SIG_UNBLOCK, &blockset, NULL);

    sigset_t sset;
    sigemptyset(&sset);
    sigaddset(&sset, SIGUSR1);
    sigaddset(&sset, SIGINT);

    struct sigaction act;
    act.sa_mask = sset;
    act.sa_handler = sig;

    if(sigaction(SIGUSR1, &act, NULL))
    {
        printf("error setting up exception handler\n");
    }
    if(sigaction(SIGINT, &act, NULL))
    {
        printf("error setting up exception handler\n");
    }
}

void formatOutFileName(char* path, const char *const format)
{
    path = dirname(path);
    strcat(path, "/");
    size_t lendir = strlen(path);

    strcat(path, format);

    unsigned short i;
    for(i = 0; wildcards[i].replacement!=NULL; i++)
    {
        // find placeholder
        char * pch = strstr(path+lendir, wildcards[i].wildcard);
        if(pch)
        {
            size_t lenrepl = strlen(wildcards[i].replacement);
            size_t lenwildc= strlen(wildcards[i].wildcard);

            // backup of everything that follows the wildcard
            char buf[PATH_MAX];
            memset(buf, '\0', PATH_MAX);
            strcpy(buf, pch+lenwildc);

            // write the replacement for the current wildcard to the path
            strncpy(pch, wildcards[i].replacement, lenrepl);

            // clear everything after the replacement
            char * h=pch+lenrepl;
            h-=1;
            while(h++ < path + PATH_MAX+1)
            {
                *h='\0';
            }

            // append the backup the path
            strcat(pch,buf);
        }
    }
}

static const struct option long_options[] =
{
    /* These options set a flag. */
    {"hle",             no_argument, (int*)&use_audiohle, 1},
//#ifdef FLAC_SUPPORT
    {"flac",            no_argument, (int*)&useFlac, 1}, // set useFlac to 1 if flac specified
//#endif
    /* These options don’t set a flag.
       We distinguish them by their indices. */
    {"fade-type",       required_argument, NULL, 'f'},
    {"play-time",       required_argument, NULL, 0},
    {"fade-time",       required_argument, NULL, 0},

    {"round-frequency", no_argument,       NULL, 'r'},
//#ifdef PLAYBACK_SUPPORT
    {"playback",        no_argument,       NULL, 'p'},
//#endif
    {"interpreter",     no_argument,       NULL, 'i'},
    {"forever",         no_argument,       NULL, 'e'},
    {"double",          no_argument,       NULL, 'd'},
    {"loop",            no_argument,       NULL, 'L'},
    {"help",            no_argument,       NULL, 'h'},
    {0, 0, 0, 0}
};

void usage(char progName[])
{
    printf("Usage: %s [OPTIONS] filename\n",progName);
    printf("\tfilename: A USF or miniUSF file\n");
    printf("\tThe output is written to filename.au\n\n");

    printf("\tOptions:\n");
    printf("\t-o\t\t\t\t specifies output filename; you may use placeholders (e.g. \"%%game%% - %%title%%\", available placeholders listed below)\n");
    printf("\t-%c\t--%s\t changes sampling rate to a more standard value, rather than the odd values that games use\n",(char)long_options[5].val,long_options[5].name);
    printf("\t-%c NUM\t--%s NUM\t\t NUM specifies the fade type: 1 - Linear; 2 - Logarithmic; 3 - half of sinewave; default: no fading\n",(char)long_options[2].val,long_options[2].name);
#ifdef FLAC_SUPPORT
    printf("\t \t--%s\t\t\t output is written to FLAC file\n",long_options[1].name);
#endif // FLAC_SUPPORT
#ifdef PLAYBACK_SUPPORT
    printf("\t \t--%s\t\t on-the-fly playback, you might hear some interrupts\n",long_options[6].name);
#endif // PLAYBACK_SUPPORT
    printf("\t \t--%s\t\t\t use high level audio emulation, will speed up emulation, at the expense of accuracy, and potential emulation bugs\n",long_options[0].name);
    printf("\t \t--%s\t\t play forever\n",long_options[8].name);
    printf("\t \t--%s SEC\t\t set playing duration to SEC seconds\n",long_options[3].name);
    printf("\t \t--%s SEC\t\t set fading duration to SEC seconds\n",long_options[4].name);
    printf("\t \t--%s\t\t double the playing length read from usf\n",long_options[9].name);
    printf("\t \t--%s\t\t use interpreter, slows down emulation; use it if recompiler (default) fails\n\n",long_options[7].name);

    puts("Available placeholders for output filename: (each placeholder should only appear once in your output filename)");

    unsigned short i;
    for(i=0; wildcards[i].replacement!=NULL; i++)
    {
        printf("\t%s\n",wildcards[i].wildcard);
    }
    puts("");
}

extern uint32_t CPU_Type;
extern int32_t RSP_Cpu;
int main(int argc, char** argv)
{
    if(argc<2)
    {
        usage(argv[0]);
        return 1;
    }

    InitSigHandler();

    /* getopt_long stores the option index here. */
    int option_index = 0;

    char* formatStr = NULL;
    bool doublePlayLength = false;

    int ch;
    while((ch = getopt_long(argc, argv, "o:f:rpiedLh", long_options, &option_index)) != -1)
    {
        switch (ch)
        {
        case 0:
            /* If this option set a flag, do nothing else now. */
            if (long_options[option_index].flag != 0)
            {
                break;
            }
            printf ("option %s", long_options[option_index].name);
            if (optarg)
            {
                printf (" with arg %s", optarg);
            }
            printf ("\n");
            break;

        case 'r':
            round_frequency=1;
            break;

        case 'p':
            playingback=1;
            break;

        case 'i':
            CPU_Type = CPU_Interpreter;
            RSP_Cpu = CPU_Interpreter;
            break;

        case 'e': // play forever
            playForever = true; /* global */
            break;

        case 'd': // double length
            doublePlayLength = true;
            break;

        case 'L': // loop playlist
            loopPlaylist = true;
            break;

        case 'f':
            fade_type=atoi(optarg);
            break;

        case 'o':
            formatStr = malloc(strlen(optarg)+1);
            strcpy(formatStr, optarg);
            break;

        case 'h':
            usage(argv[0]);
            return 0;
            break;

        case '?':
            /* getopt_long already printed an error message. */
            break;

        default:
            abort();
        }
    }

    if(optind >= argc)
    {
        puts("No path was specified");
        return 1;
    }

    InitAudio();

    if(isatty(STDOUT_FILENO))
        printf("lazyusf v%s\n\n", VERSION);

    pthread_t key_tid;
    bool key_thread_running = false;
    pthread_t status_tid;
    bool status_thread_running = false;
    if(isatty(STDIN_FILENO))
    {
        enable_raw_mode();
        pthread_create(&key_tid, NULL, key_reader, NULL);
        key_thread_running = true;
    }
    if(isatty(STDOUT_FILENO))
    {
        pthread_create(&status_tid, NULL, status_thread, NULL);
        status_thread_running = true;
    }

    int first_index = optind;
    int i = optind;
    bool first_track = true;
    while(i < argc && !quit_all)
    {
        go_prev = false;

        if(!realpath(argv[i], filename))
        {
            printf("Failed to get the full path of \"%s\". Does it exist?\n", argv[i]);
            i++;
            continue;
        }

        if(usf_init(filename))
        {
            saved_track_time = track_time;
            if(playForever)
                track_time |= 1u << (sizeof(uint32_t)*8 - 1);
#define TRACK_LINES 7
            pthread_mutex_lock(&term_mutex);
            if(!first_track && isatty(STDOUT_FILENO))
                printf("\033[%dF", TRACK_LINES);
            first_track = false;

            printf("%-12s%s\033[K\n", "Game:", game);
            printf("%-12s%s\033[K\n", "Title:", title);
            printf("%-12s%s\033[K\n", "Artist:", artist);
            printf("%-12s%s\033[K\n", "Genre:", genre);
            printf("%-12s%s\033[K\n", "Copyright:", copyright);
            printf("%-12s%s\033[K\n", "Year:", year);
            printf("\033[K\n");
            display_ready = true;
            print_status();
            pthread_mutex_unlock(&term_mutex);

            if(doublePlayLength)
            {
                track_time *= 2;
            }

            if(formatStr)
            {
                formatOutFileName(filename, formatStr);
            }

            if(useFlac)
            {
                strcat(filename,".flac");
            }
            else
            {
                strcat(filename,".au");
            }

            if(!usf_play())
            {
                printf("An Error occured while play.\n");
            }
        }
        else
        {
            printf("An Error occured while init.\n");
        }

        if(go_prev && i > first_index)
            i--;
        else if(++i >= argc && loopPlaylist)
            i = first_index;
    }

    if(status_thread_running)
    {
        pthread_cancel(status_tid);
        pthread_join(status_tid, NULL);
        if(isatty(STDOUT_FILENO)) printf("\n");
    }

    if(key_thread_running)
    {
        pthread_cancel(key_tid);
        pthread_join(key_tid, NULL);
        disable_raw_mode();
    }

    DoneAudio();
    free(formatStr);

    return 0;
}
