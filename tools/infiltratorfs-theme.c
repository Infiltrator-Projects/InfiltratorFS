// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratr/design.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void print_colour(const char *name, uint32_t rgb)
{
    printf("%s=#%06x\n", name, (unsigned)(rgb & UINT32_C(0x00ffffff)));
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: infiltratorfs-theme system|day|night [light|dark]\n");
        return 2;
    }

    InfiltratrThemeMode mode;
    if (strcmp(argv[1], "system") == 0)
        mode = INFILTRATR_THEME_SYSTEM;
    else if (strcmp(argv[1], "day") == 0)
        mode = INFILTRATR_THEME_DAY;
    else if (strcmp(argv[1], "night") == 0)
        mode = INFILTRATR_THEME_NIGHT;
    else
        return 2;

    bool system_dark = argc == 3 && strcmp(argv[2], "dark") == 0;
    if (argc == 3 && !system_dark && strcmp(argv[2], "light") != 0)
        return 2;

    const InfiltratrThemePalette *p =
        infiltratr_theme_resolve(mode, system_dark);
    if (!p)
        return 1;

#define PRINT_COLOUR(field) print_colour(#field, p->field##_rgb)
    PRINT_COLOUR(background);
    PRINT_COLOUR(panel);
    PRINT_COLOUR(card);
    PRINT_COLOUR(surface);
    PRINT_COLOUR(input);
    PRINT_COLOUR(border);
    PRINT_COLOUR(text);
    PRINT_COLOUR(title);
    PRINT_COLOUR(muted);
    PRINT_COLOUR(subtle);
    PRINT_COLOUR(button_background);
    PRINT_COLOUR(button_foreground);
    PRINT_COLOUR(selection_background);
    PRINT_COLOUR(selection_foreground);
    PRINT_COLOUR(neutral_accent);
    PRINT_COLOUR(success);
    PRINT_COLOUR(warning);
    PRINT_COLOUR(fault);
    PRINT_COLOUR(info);
    PRINT_COLOUR(operation);
    PRINT_COLOUR(card_hover);
    PRINT_COLOUR(surface_hover);
    PRINT_COLOUR(operation_hover);
    PRINT_COLOUR(equals_hover);
#undef PRINT_COLOUR
    return 0;
}
