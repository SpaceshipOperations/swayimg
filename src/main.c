// SPDX-License-Identifier: MIT
// Program entry point.
// Copyright (C) 2020 Artem Senichev <artemsen@gmail.com>

#include "buildcfg.h"
#include "canvas.h"
#include "config.h"
#include "font.h"
#include "formats/loader.h"
#include "imagelist.h"
#include "info.h"
#include "keybind.h"
#include "sway.h"
#include "ui.h"
#include "viewer.h"

#include <getopt.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Command line options. */
struct cmdarg {
    const char short_opt; ///< Short option character
    const char* long_opt; ///< Long option name
    const char* format;   ///< Format description
    const char* help;     ///< Help string
    const char* section;  ///< Section name of the param
    const char* key;      ///< Key of the param
    const char* value;    ///< Static value to set
};

// clang-format off
static const struct cmdarg arguments[] = {
    { 'r', "recursive",  NULL,    "read directories recursively",
                                  INFO_CFG_SECTION, INFO_CFG_RECURSIVE, "yes" },
    { 'o', "order",      "ORDER", "set sort order for image list: none/[alpha]/random",
                                  INFO_CFG_SECTION, INFO_CFG_ORDER, NULL },
    { 's', "scale",      "SCALE", "set initial image scale: [optimal]/fit/width/height/fill/real",
                                  GENERAL_CONFIG_SECTION, CANVAS_CFG_SCALE, NULL },
    { 'l', "slideshow",  NULL,    "activate slideshow mode on startup",
                                  GENERAL_CONFIG_SECTION, VIEWER_CFG_SLIDESHOW, "yes" },
    { 'f', "fullscreen", NULL,    "show image in full screen mode",
                                  GENERAL_CONFIG_SECTION, UI_CFG_FULLSCREEN, "yes" },
    { 'p', "position",   "POS",   "set window position [parent]/X,Y",
                                  GENERAL_CONFIG_SECTION, UI_CFG_POSITION, NULL },
    { 'g', "size",       "SIZE",  "set window size: [parent]/image/W,H",
                                  GENERAL_CONFIG_SECTION, UI_CFG_SIZE, NULL },
    { 'a', "class",      "NAME",  "set window class/app_id",
                                  GENERAL_CONFIG_SECTION, UI_CFG_APP_ID, NULL },
    { 'c', "config",     "S.K=V", "set configuration parameter: section.key=value",
                                  NULL, NULL, NULL },
    { 'v', "version",    NULL,    "print version info and exit", NULL, NULL, NULL },
    { 'h', "help",       NULL,    "print this help and exit", NULL, NULL, NULL }
};
// clang-format on

/**
 * Print usage info.
 */
static void print_help(void)
{
    char buf_lopt[32];

    puts("Usage: " APP_NAME " [OPTION]... [FILE]...");
    puts("Show images from FILE(s).");
    puts("If FILE is -, read standard input.");
    puts("If no FILE specified - read all files from the current directory.\n");
    puts("Mandatory arguments to long options are mandatory for short options "
         "too.");

    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        const struct cmdarg* arg = &arguments[i];
        strcpy(buf_lopt, arg->long_opt);
        if (arg->format) {
            strcat(buf_lopt, "=");
            strcat(buf_lopt, arg->format);
        }
        printf("  -%c, --%-18s %s\n", arg->short_opt, buf_lopt, arg->help);
    }
}

/**
 * Parse command line arguments into configuration instance.
 * @param argc number of arguments to parse
 * @param argv arguments array
 * @return index of the first non option argument, or -1 if error, or 0 to exit
 */
static int parse_cmdargs(int argc, char* argv[])
{
    struct option options[1 + (sizeof(arguments) / sizeof(arguments[0]))];
    char short_opts[(sizeof(arguments) / sizeof(arguments[0])) * 2];
    char* short_opts_ptr = short_opts;
    int opt;

    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        const struct cmdarg* arg = &arguments[i];
        // compose array of option structs
        options[i].name = arg->long_opt;
        options[i].has_arg = arg->format ? required_argument : no_argument;
        options[i].flag = NULL;
        options[i].val = arg->short_opt;
        // compose short options string
        *short_opts_ptr++ = arg->short_opt;
        if (arg->format) {
            *short_opts_ptr++ = ':';
        }
    }
    // add terminations
    *short_opts_ptr = 0;
    memset(&options[(sizeof(arguments) / sizeof(arguments[0]))], 0,
           sizeof(struct option));

    // parse arguments
    while ((opt = getopt_long(argc, argv, short_opts, options, NULL)) != -1) {
        const struct cmdarg* arg;
        if (opt == '?') {
            return -1;
        }
        // get argument description
        arg = arguments;
        while (arg->short_opt != opt) {
            ++arg;
        }
        if (arg->section) {
            if (config_set(arg->section, arg->key,
                           arg->value ? arg->value : optarg) != cfgst_ok) {
                return -1;
            }
            continue;
        }
        switch (opt) {
            case 'c':
                if (!config_command(optarg)) {
                    return -1;
                }
                break;
            case 'v':
                puts(APP_NAME " version " APP_VERSION ".");
                puts("https://github.com/artemsen/swayimg");
                printf("Supported formats: %s.\n", supported_formats);
                return 0;
            case 'h':
                print_help();
                return 0;
            default:
                return -1;
        }
    }

    return optind;
}

/**
 * Setup window position via Sway IPC.
 */
static void sway_setup(void)
{
    struct rect parent;
    bool fullscreen;
    bool rc = false;
    const int ipc = sway_connect();

    if (ipc != INVALID_SWAY_IPC && sway_current(ipc, &parent, &fullscreen)) {
        if (fullscreen && !ui_get_fullscreen()) {
            ui_toggle_fullscreen();
        }
        if (!ui_get_fullscreen()) {
            const bool absolute =
                ui_get_x() != POS_FROM_PARENT && ui_get_y() != POS_FROM_PARENT;

            if (!absolute) {
                ui_set_position(parent.x, parent.y);
            }

            if (ui_get_width() == SIZE_FROM_PARENT ||
                ui_get_height() == SIZE_FROM_PARENT) {
                ui_set_size(parent.width, parent.height);
            }

            rc = sway_add_rules(ipc, ui_get_appid(), ui_get_x(), ui_get_y(),
                                absolute);
        }
    }

    sway_disconnect(ipc);

    if (!rc) {
        // set fixed app_id
        config_set(GENERAL_CONFIG_SECTION, "app_id", APP_NAME);

        // fixup window size
        if (ui_get_width() == SIZE_FROM_PARENT ||
            ui_get_height() == SIZE_FROM_PARENT) {
            const struct image_frame* frame =
                image_list_current().image->frames;
            ui_set_size(frame->width, frame->height);
        }
    }
}

/**
 * Application entry point.
 */
int main(int argc, char* argv[])
{
    bool rc = false;
    int argn;

    setlocale(LC_ALL, "");

    keybind_init();
    font_init();
    info_init();
    image_list_init();
    canvas_init();
    ui_init();
    viewer_init();
    config_init();

    // parse command arguments
    argn = parse_cmdargs(argc, argv);
    if (argn <= 0) {
        rc = (argn == 0);
        goto done;
    }

    // compose file list
    if (!image_list_scan((const char**)&argv[argn], argc - argn)) {
        fprintf(stderr, "No images to view, exit\n");
        goto done;
    }

    // set window size form the first image
    if (ui_get_width() == SIZE_FROM_IMAGE ||
        ui_get_height() == SIZE_FROM_IMAGE) {
        const struct image_frame* frame = image_list_current().image->frames;
        ui_set_size(frame->width, frame->height);
    }

    sway_setup();

    // run ui event loop
    rc = ui_run();

done:
    config_free();
    viewer_free();
    ui_free();
    image_list_free();
    info_free();
    font_free();
    keybind_free();

    return rc ? EXIT_SUCCESS : EXIT_FAILURE;
}
