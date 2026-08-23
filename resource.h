/* resource.h - shared IDs for Bildschirmschoner-Steuerung / Screen Saver Control */
#ifndef RESOURCE_H
#define RESOURCE_H

/* ---- version ---------------------------------------------------------- */
#define VER_MAJOR   1
#define VER_MINOR   0
#define VER_REV     0
#define VER_BUILD   0
#define VER_STR     "1.0.0.0"

/* Homepage opened by the link in the About box. Change to taste. */
#define APP_URL     "https://www.rekow.ch"

/* ---- icons ------------------------------------------------------------ */
#define IDI_ENABLED       1
#define IDI_DISABLED      2

/* ---- menus ------------------------------------------------------------ */
#define IDR_MENU_DE       132
#define IDR_MENU_EN       133

/* positions of the three dynamic popups inside the tray popup menu */
#define POS_RUN           1
#define POS_CONFIG        2
#define POS_SETDEFAULT    3

/* ---- dialogs ---------------------------------------------------------- */
#define IDD_ABOUT_DE      101
#define IDD_ABOUT_EN      102

#define IDC_ABOUT_TEXT    1031
#define IDC_ABOUT_COPY    1000
#define IDC_ABOUT_WARN    1002
#define IDC_ABOUT_LINK    1010
#define IDC_ABOUT_ICON    1043

/* ---- commands (as defined in the original Menu.rc) --------------------- */
#define IDM_START_DEFAULT 32771
#define IDM_EXIT          32773
#define IDM_ABOUT         32780
#define IDM_DISABLE       32781
#define IDM_ENABLE        32782
#define IDM_DISPLAY       32784
#define IDM_AUTOSTART     32785
#define IDM_NONE_RUN      32786
#define IDM_NONE_CFG      32787
#define IDM_NONE_SET      32788
#define IDM_LANG_DE       32790
#define IDM_LANG_EN       32791

/* ---- dynamic command ranges ------------------------------------------- */
#define IDR_RUN_BASE      40000
#define IDR_CFG_BASE      41000
#define IDR_SET_BASE      42000
#define IDR_RANGE         900

/* ---- string table ----------------------------------------------------- */
#define IDS_BASE_DE       1000
#define IDS_BASE_EN       1100

#define IDS_NONE          0
#define IDS_TIP_ON        1
#define IDS_TIP_OFF       2
#define IDS_NODEFAULT     3
#define IDS_APPTITLE      4
#define IDS_STARTFAIL     5

#endif /* RESOURCE_H */
