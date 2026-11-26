#ifndef FLEX_LAUNCHER_APPEARANCE_H
#define FLEX_LAUNCHER_APPEARANCE_H

#define ROW_POSITION_AUTO (-1)
#define CLASSIC_ROW_MIN 25
#define CLASSIC_ROW_MAX 75
#define CLASSIC_ROW_DEFAULT 50
#define TV_CARD_SCALE_MIN 70
#define TV_CARD_SCALE_MAX 120
#define TV_CARD_SCALE_DEFAULT 100
#define TV_CARD_OPACITY_DEFAULT 83

typedef struct {
    int row_position; // Centre of the row as a percentage of screen height.
    int card_scale;
    int card_opacity;
} AppearanceSettings;

#endif
