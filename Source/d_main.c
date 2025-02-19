// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// $Id: d_main.c,v 1.47 1998/05/16 09:16:51 killough Exp $
//
//  Copyright (C) 1999 by
//  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 
//  02111-1307, USA.
//
// DESCRIPTION:
//  DOOM main program (D_DoomMain) and game loop, plus functions to
//  determine game mode (shareware, registered), parse command line
//  parameters, configure game parameters (turbo), and call the startup
//  functions.
//
//-----------------------------------------------------------------------------

#include "d_io.h" // haleyjd
#include "SDL_filesystem.h" // [FG] SDL_GetPrefPath()
#include "SDL_stdinc.h" // [FG] SDL_qsort()

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "doomdef.h"
#include "doomstat.h"
#include "dstrings.h"
#include "sounds.h"
#include "z_zone.h"
#include "w_wad.h"
#include "s_sound.h"
#include "v_video.h"
#include "f_finale.h"
#include "f_wipe.h"
#include "m_argv.h"
#include "m_misc.h"
#include "m_misc2.h" // [FG] M_StringDuplicate()
#include "m_menu.h"
#include "i_system.h"
#include "i_sound.h"
#include "i_video.h"
#include "g_game.h"
#include "hu_stuff.h"
#include "wi_stuff.h"
#include "st_stuff.h"
#include "am_map.h"
#include "p_setup.h"
#include "r_draw.h"
#include "r_main.h"
#include "d_main.h"
#include "d_iwad.h" // [FG] BuildIWADDirList()
#include "d_deh.h"  // Ty 04/08/98 - Externalizations
#include "statdump.h" // [FG] StatDump()
#include "u_mapinfo.h" // U_ParseMapInfo()
#include "i_glob.h" // [FG] I_StartMultiGlob()
#include "p_map.h" // MELEERANGE

#include "dsdhacked.h"

#include "net_client.h"

#ifdef _WIN32
#include "../win32/win_fopen.h"
#endif

// DEHacked support - Ty 03/09/97
// killough 10/98:
// Add lump number as third argument, for use when filename==NULL
void ProcessDehFile(const char *filename, char *outfilename, int lump);

// mbf21
void PostProcessDeh(void);

// killough 10/98: support -dehout filename
static char *D_dehout(void)
{
  static char *s;      // cache results over multiple calls
  if (!s)
    {
      int p = M_CheckParm("-dehout");
      if (!p)
        p = M_CheckParm("-bexout");
      s = p && ++p < myargc ? myargv[p] : "";
    }
  return s;
}

char **wadfiles;

// killough 10/98: preloaded files
#define MAXLOADFILES 2
char *wad_files[MAXLOADFILES], *deh_files[MAXLOADFILES];

boolean devparm;        // started game with -devparm

// jff 1/24/98 add new versions of these variables to remember command line
boolean clnomonsters;   // checkparm of -nomonsters
boolean clrespawnparm;  // checkparm of -respawn
boolean clfastparm;     // checkparm of -fast
// jff 1/24/98 end definition of command line version of play mode switches

boolean nomonsters;     // working -nomonsters
boolean respawnparm;    // working -respawn
boolean fastparm;       // working -fast
boolean pistolstart;    // working -pistolstart

boolean singletics = false; // debug flag to cancel adaptiveness

//jff 1/22/98 parms for disabling music and sound
boolean nosfxparm;
boolean nomusicparm;

//jff 4/18/98
extern boolean inhelpscreens;

skill_t startskill;
int     startepisode;
int     startmap;
boolean autostart;
FILE    *debugfile;
int     startloadgame;

boolean advancedemo;

char    *basedefault = NULL;   // default file
char    *basesavegame = NULL;  // killough 2/16/98: savegame directory

// If true, the main game loop has started.
boolean main_loop_started = false;

boolean coop_spawns = false;

//jff 4/19/98 list of standard IWAD names
typedef struct
{
    const char *name;
    GameMission_t mission;
    GameMode_t mode;
} iwad_t;

static iwad_t standard_iwads[] =
{
  { "doom2.wad",     doom2,      commercial },
  { "plutonia.wad",  pack_plut,  commercial },
  { "tnt.wad",       pack_tnt,   commercial },
  { "doom.wad",      doom,       retail },
  { "doom1.wad",     doom,       shareware },
  { "doom2f.wad",    doom2,      commercial },
  { "chex.wad",      pack_chex,  retail },
  { "hacx.wad",      pack_hacx,  commercial },
  { "freedoom2.wad", doom2,      commercial },
  { "freedoom1.wad", doom,       retail },
  { "freedm.wad",    doom2,      commercial },
  { "rekkrsa.wad",   pack_rekkr, retail }
};

void D_ConnectNetGame (void);
void D_CheckNetGame (void);
void D_ProcessEvents (void);
void G_BuildTiccmd (ticcmd_t* cmd);
void D_DoAdvanceDemo (void);

//
// EVENT HANDLING
//
// Events are asynchronous inputs generally generated by the game user.
// Events can be discarded if no responder claims them
//

event_t events[MAXEVENTS];
int eventhead, eventtail;

//
// D_PostEvent
// Called by the I/O functions when input is detected
//
void D_PostEvent(event_t *ev)
{
  events[eventhead++] = *ev;
  eventhead &= MAXEVENTS-1;
}

//
// D_ProcessEvents
// Send all the events of the given timestamp down the responder chain
//

void D_ProcessEvents (void)
{
  // IF STORE DEMO, DO NOT ACCEPT INPUT
  if (gamemode != commercial || W_CheckNumForName("map01") >= 0)
    for (; eventtail != eventhead; eventtail = (eventtail+1) & (MAXEVENTS-1))
    {
      M_InputTrackEvent(events+eventtail);
      if (!M_Responder(events+eventtail))
        G_Responder(events+eventtail);
    }
}

//
// D_Display
//  draw current display, possibly wiping it from the previous
//

// wipegamestate can be set to -1 to force a wipe on the next draw
gamestate_t    wipegamestate = GS_DEMOSCREEN;
extern int     showMessages;

void D_Display (void)
{
  static boolean viewactivestate = false;
  static boolean menuactivestate = false;
  static boolean inhelpscreensstate = false;
  static boolean fullscreen = false;
  static gamestate_t oldgamestate = -1;
  static int borderdrawcount;
  int wipestart;
  boolean done, wipe, redrawsbar;

  if (nodrawers)                    // for comparative timing / profiling
    return;

  redrawsbar = false;

  if (setsizeneeded)                // change the view size if needed
    {
      R_ExecuteSetViewSize();
      oldgamestate = -1;            // force background redraw
      borderdrawcount = 3;
    }

  // save the current screen if about to wipe
  if ((wipe = gamestate != wipegamestate))
    wipe_StartScreen(0, 0, SCREENWIDTH, SCREENHEIGHT);

  if (gamestate == GS_LEVEL && gametic)
    HU_Erase();

  switch (gamestate)                // do buffered drawing
    {
    case GS_LEVEL:
      if (!gametic)
        break;
      if (automapactive)
      {
        // [FG] update automap while playing
        R_RenderPlayerView (&players[displayplayer]);
        AM_Drawer();
      }
      if (wipe || (scaledviewheight != 200 && fullscreen) // killough 11/98
          || (inhelpscreensstate && !inhelpscreens))
        redrawsbar = true;              // just put away the help screen
      ST_Drawer(scaledviewheight == 200, redrawsbar );    // killough 11/98
      fullscreen = scaledviewheight == 200;               // killough 11/98
      break;
    case GS_INTERMISSION:
      WI_Drawer();
      break;
    case GS_FINALE:
      F_Drawer();
      break;
    case GS_DEMOSCREEN:
      D_PageDrawer();
      break;
    }

  // draw buffered stuff to screen
  I_UpdateNoBlit();

  // draw the view directly
  if (gamestate == GS_LEVEL && !automapactive && gametic)
    R_RenderPlayerView (&players[displayplayer]);

  if (gamestate == GS_LEVEL && gametic)
    HU_Drawer ();

  // clean up border stuff
  if (gamestate != oldgamestate && gamestate != GS_LEVEL)
    I_SetPalette (W_CacheLumpName ("PLAYPAL",PU_CACHE));

  // see if the border needs to be initially drawn
  if (gamestate == GS_LEVEL && oldgamestate != GS_LEVEL)
    {
      viewactivestate = false;        // view was not active
      R_FillBackScreen ();    // draw the pattern into the back screen
    }

  // see if the border needs to be updated to the screen
  if (gamestate == GS_LEVEL && (!automapactive || automapoverlay) && scaledviewwidth != 320)
    {
      if (menuactive || menuactivestate || !viewactivestate)
        borderdrawcount = 3;
      if (borderdrawcount)
        {
          R_DrawViewBorder ();    // erase old menu stuff
          borderdrawcount--;
        }
    }

  menuactivestate = menuactive;
  viewactivestate = viewactive;
  inhelpscreensstate = inhelpscreens;
  oldgamestate = wipegamestate = gamestate;

  if (gamestate == GS_LEVEL && automapactive && automapoverlay)
    {
      AM_Drawer();
      HU_Drawer();

      // [crispy] force redraw of status bar and border
      viewactivestate = false;
      inhelpscreensstate = true;
    }

  // draw pause pic
  if (paused)
    {
      int y = 4;
      if (!automapactive)
        y += (viewwindowy>>hires);
      V_DrawPatchDirect((viewwindowx>>hires)+(scaledviewwidth-68)/2-WIDESCREENDELTA,
                        y,0,W_CacheLumpName ("M_PAUSE", PU_CACHE));
    }

  // menus go directly to the screen
  M_Drawer();          // menu is drawn even on top of everything
  NetUpdate();         // send out any new accumulation

  // normal update
  if (!wipe)
    {
      I_FinishUpdate ();              // page flip or blit buffer
      return;
    }

  // wipe update
  wipe_EndScreen(0, 0, SCREENWIDTH, SCREENHEIGHT);

  wipestart = I_GetTime () - 1;

  do
    {
      int nowtime, tics;
      do
        {
          nowtime = I_GetTime();
          tics = nowtime - wipestart;
        }
      while (!tics);
      wipestart = nowtime;
      done = wipe_ScreenWipe(wipe_Melt,0,0,SCREENWIDTH,SCREENHEIGHT,tics);
      I_UpdateNoBlit();
      M_Drawer();                   // menu is drawn even on top of wipes
      I_FinishUpdate();             // page flip or blit buffer
    }
  while (!done);
}

//
//  DEMO LOOP
//

static int demosequence;         // killough 5/2/98: made static
static int pagetic;
static char *pagename;

//
// D_PageTicker
// Handles timing for warped projection
//
void D_PageTicker(void)
{
  // killough 12/98: don't advance internal demos if a single one is 
  // being played. The only time this matters is when using -loadgame with
  // -fastdemo, -playdemo, or -timedemo, and a consistency error occurs.

  if (!singledemo && --pagetic < 0)
    D_AdvanceDemo();
}

//
// D_PageDrawer
//
// killough 11/98: add credits screen
//

void D_PageDrawer(void)
{
  if (pagename)
    {
      int l = W_CheckNumForName(pagename);
      byte *t = W_CacheLumpNum(l, PU_CACHE);
      size_t s = W_LumpLength(l);
      unsigned c = 0;
      while (s--)
	c = c*3 + t[s];
      V_DrawPatchFullScreen(0, (patch_t *) t);
      if (c==2119826587u || c==2391756584u)
        // [FG] removed the embedded DOGOVRLY title pic overlay graphic lump
        if (W_CheckNumForName("DOGOVRLY") > 0)
        {
	V_DrawPatch(0, 0, 0, W_CacheLumpName("DOGOVRLY", PU_CACHE));
        }
    }
  else
    M_DrawCredits();
}

//
// D_AdvanceDemo
// Called after each demo or intro demosequence finishes
//

void D_AdvanceDemo (void)
{
  advancedemo = true;
}

// killough 11/98: functions to perform demo sequences

static void D_SetPageName(char *name)
{
  pagename = name;
}

static void D_DrawTitle1(char *name)
{
  S_StartMusic(mus_intro);
  pagetic = (TICRATE*170)/35;
  D_SetPageName(name);
}

static void D_DrawTitle2(char *name)
{
  S_StartMusic(mus_dm2ttl);
  D_SetPageName(name);
}

// killough 11/98: tabulate demo sequences

static struct 
{
  void (*func)(char *);
  char *name;
} const demostates[][4] =
  {
    {
      {D_DrawTitle1, "TITLEPIC"},
      {D_DrawTitle1, "TITLEPIC"},
      {D_DrawTitle2, "TITLEPIC"},
      {D_DrawTitle1, "TITLEPIC"},
    },

    {
      {G_DeferedPlayDemo, "demo1"},
      {G_DeferedPlayDemo, "demo1"},
      {G_DeferedPlayDemo, "demo1"},
      {G_DeferedPlayDemo, "demo1"},
    },

    // [FG] swap third and fifth state in the sequence,
    //      so that a WAD's credit screen gets precedence over Woof!'s own
    //      (also, show the credit screen for The Ultimate Doom)
    {
      {D_SetPageName, "HELP2"},
      {D_SetPageName, "HELP2"},
      {D_SetPageName, "CREDIT"},
      {D_SetPageName, "CREDIT"},
    },

    {
      {G_DeferedPlayDemo, "demo2"},
      {G_DeferedPlayDemo, "demo2"},
      {G_DeferedPlayDemo, "demo2"},
      {G_DeferedPlayDemo, "demo2"},
    },

    // [FG] swap third and fifth state in the sequence,
    //      so that a WAD's credit screen gets precedence over Woof!'s own
    {
      {D_SetPageName, NULL},
      {D_SetPageName, NULL},
      {D_SetPageName, NULL},
      {D_SetPageName, NULL},
    },

    {
      {G_DeferedPlayDemo, "demo3"},
      {G_DeferedPlayDemo, "demo3"},
      {G_DeferedPlayDemo, "demo3"},
      {G_DeferedPlayDemo, "demo3"},
    },

    {
      {NULL},
      {NULL},
      {NULL},
      {D_SetPageName, "CREDIT"},
    },

    {
      {NULL},
      {NULL},
      {NULL},
      {G_DeferedPlayDemo, "demo4"},
    },

    {
      {NULL},
      {NULL},
      {NULL},
      {NULL},
    }
  };

//
// This cycles through the demo sequences.
//
// killough 11/98: made table-driven

void D_DoAdvanceDemo(void)
{
  char *name;
  players[consoleplayer].playerstate = PST_LIVE;  // not reborn
  advancedemo = usergame = paused = false;
  gameaction = ga_nothing;

  pagetic = TICRATE * 11;         // killough 11/98: default behavior
  gamestate = GS_DEMOSCREEN;

  if (!demostates[++demosequence][gamemode].func)
    demosequence = 0;
  // [FG] the BFG Edition IWADs have no TITLEPIC lump, use DMENUPIC instead
  name = demostates[demosequence][gamemode].name;
  if (name && !strcasecmp(name, "TITLEPIC"))
  {
    int i = W_CheckNumForName("TITLEPIC");
    int j = W_CheckNumForName("DMENUPIC");

    if (i < 0 || (j >= 0 && W_IsIWADLump(i)))
      name = (j >= 0) ? "DMENUPIC" : "INTERPIC";
  }
  demostates[demosequence][gamemode].func
    (name);
}

//
// D_StartTitle
//
void D_StartTitle (void)
{
  gameaction = ga_nothing;
  demosequence = -1;
  D_AdvanceDemo();
}

// print title for every printed line
static char title[128];

//
// D_AddFile
//
// Rewritten by Lee Killough
//
// killough 11/98: remove limit on number of files
//

void D_AddFile(const char *file)
{
  static int numwadfiles, numwadfiles_alloc;

  if (numwadfiles >= numwadfiles_alloc)
    wadfiles = realloc(wadfiles, (numwadfiles_alloc = numwadfiles_alloc ?
                                  numwadfiles_alloc * 2 : 8)*sizeof*wadfiles);
  // [FG] search for PWADs by their filename
  wadfiles[numwadfiles++] = !file ? NULL : D_TryFindWADByName(file);
}

// Return the path where the executable lies -- Lee Killough
char *D_DoomExeDir(void)
{
   // haleyjd: modified to prevent returning empty string
   static char *base;
   if(!base)        // cache multiple requests
   {
      size_t len = strlen(*myargv) + 1;
      char *p;

      base = malloc(len);
      memset(base, 0, len);

      p = base + len - 1;
      
      strncpy(base, *myargv, len);
      
      while(p >= base)
      {
         if(*p == '/' || *p == '\\')
         {
            *p = '\0';
            break;
         }
         *p = '\0';
         p--;
      }
   }

   if(*base == '\0')
      *base = '.';

   return base;
}

// killough 10/98: return the name of the program the exe was invoked as
char *D_DoomExeName(void)
{
  static char *name;    // cache multiple requests
  if (!name)
    {
      char *p = *myargv + strlen(*myargv);
      int i = 0;
      while (p > *myargv && p[-1] != '/' && p[-1] != '\\' && p[-1] != ':')
        p--;
      while (p[i] && p[i] != '.')
        i++;
      strncpy(name = malloc(i+1), p, i)[i] = 0;
    }
  return name;
}

// [FG] get the path to the default configuration dir to use

char *D_DoomPrefDir(void)
{
    static char *dir;

    if (dir == NULL)
    {
        char *result;

#if !defined(_WIN32) || defined(_WIN32_WCE)
        // Configuration settings are stored in an OS-appropriate path
        // determined by SDL.  On typical Unix systems, this might be
        // ~/.local/share/chocolate-doom.  On Windows, we behave like
        // Vanilla Doom and save in the current directory.

        result = SDL_GetPrefPath("", PROJECT_TARNAME);
        if (result != NULL)
        {
            dir = M_StringDuplicate(result);
            SDL_free(result);
        }
        else
#endif /* #ifndef _WIN32 */
        {
            result = D_DoomExeDir();
            dir = M_StringDuplicate(result);
        }

        M_MakeDirectory(dir);
    }

    return dir;
}

// Calculate the path to the directory for autoloaded WADs/DEHs.
// Creates the directory as necessary.

static struct {
    const char *dir;
    char *(*func)(void);
} autoload_basedirs[] = {
#ifdef WOOFDATADIR
    {WOOFDATADIR, NULL},
#endif
    {NULL, D_DoomPrefDir},
#if !defined(_WIN32) || defined(_WIN32_WCE)
    {NULL, D_DoomExeDir},
#endif
    {NULL, NULL},
};

static char **autoload_paths = NULL;

static char *GetAutoloadDir(const char *base, const char *iwadname, boolean createdir)
{
    char *result;
    char *lower;

    lower = M_StringDuplicate(iwadname);
    M_ForceLowercase(lower);
    result = M_StringJoin(base, DIR_SEPARATOR_S, lower, NULL);
    (free)(lower);

    if (createdir)
    {
        M_MakeDirectory(result);
    }

    return result;
}

static void PrepareAutoloadPaths (void)
{
    int i;

    if (M_CheckParm("-noload") || M_CheckParm("-noautoload"))
        return;

    for (i = 0; ; i++)
    {
        autoload_paths = realloc(autoload_paths, (i + 1) * sizeof(*autoload_paths));

        if (autoload_basedirs[i].dir)
        {
            autoload_paths[i] = M_StringJoin(autoload_basedirs[i].dir, DIR_SEPARATOR_S, "autoload", NULL);
        }
        else if (autoload_basedirs[i].func)
        {
            autoload_paths[i] = M_StringJoin(autoload_basedirs[i].func(), DIR_SEPARATOR_S, "autoload", NULL);
        }
        else
        {
            autoload_paths[i] = NULL;
            break;
        }

        M_MakeDirectory(autoload_paths[i]);
    }
}

//
// CheckIWAD
//

static void IdentifyVersionByContent(const char *iwadname)
{
    int i;
    FILE *file;
    wadinfo_t header;
    filelump_t *fileinfo;

    file = fopen(iwadname, "rb");

    if (file == NULL)
    {
        I_Error("CheckIWAD: failed to read IWAD %s", iwadname);
    }

    // read IWAD header
    if (fread(&header, sizeof(header), 1, file) != 1)
    {
        fclose(file);
        I_Error("CheckIWAD: failed to read header %s", iwadname);
        return;
    }

    if (strncmp(header.identification, "IWAD", 4))
    {
        printf("CheckIWAD: IWAD tag %s not present\n", iwadname);
    }

    // read IWAD directory
    header.numlumps = LONG(header.numlumps);
    header.infotableofs = LONG(header.infotableofs);
    fileinfo = (malloc)(header.numlumps * sizeof(filelump_t));

    if (fseek(file, header.infotableofs, SEEK_SET) ||
        fread(fileinfo, sizeof(filelump_t), header.numlumps, file) != header.numlumps)
    {
        (free)(fileinfo);
        fclose(file);
        I_Error("CheckIWAD: failed to read directory %s", iwadname);
        return;
    }

    for (i = 0; i < header.numlumps; ++i)
    {
        if (!strncasecmp(fileinfo[i].name, "MAP01", 8))
        {
            gamemission = doom2;
            break;
        }
        else if (!strncasecmp(fileinfo[i].name, "E1M1", 8))
        {
            gamemode = shareware;
            gamemission = doom;
            break;
        }
    }

    if (gamemission == doom2)
    {
        gamemode = commercial;
    }
    else
    {
        for (i = 0; i < header.numlumps; ++i)
        {
            if (!strncasecmp(fileinfo[i].name, "E4M1", 8))
            {
                gamemode = retail;
                break;
            }
            else if (!strncasecmp(fileinfo[i].name, "E3M1", 8))
            {
                gamemode = registered;
            }
        }
    }

    (free)(fileinfo);
    fclose(file);

    if (gamemode == indetermined)
    {
        I_Error("Unknown or invalid IWAD file.");
    }
}

static void CheckIWAD(const char *iwadname)
{
    int i;
    const char *name = M_BaseName(iwadname);

    for (i = 0; i < arrlen(standard_iwads); ++i)
    {
        if (!strcasecmp(name, standard_iwads[i].name))
        {
            gamemode = standard_iwads[i].mode;
            gamemission = standard_iwads[i].mission;
            break;
        }
    }

    if (gamemode == indetermined)
    {
        IdentifyVersionByContent(iwadname);
    }
}

//
// FindIWADFIle
//

char *FindIWADFile(void)
{
    char *result;
    int iwadparm = M_CheckParmWithArgs("-iwad", 1);

    if (iwadparm)
    {
        // Search through IWAD dirs for an IWAD with the given name.

        char *iwadfile = myargv[iwadparm + 1];

        char *file = (malloc)(strlen(iwadfile) + 5);
        AddDefaultExtension(strcpy(file, iwadfile), ".wad");

        result = D_FindWADByName(file);

        if (result == NULL)
        {
            I_Error("IWAD file '%s' not found!", file);
        }

        (free)(file);
    }
    else
    {
        int i;

        // Search through the list and look for an IWAD

        result = NULL;

        for (i = 0; result == NULL && i < arrlen(standard_iwads); ++i)
        {
            result = D_FindWADByName(standard_iwads[i].name);
        }
    }

    return result;
}

//
// IdentifyVersion
//
// Set the location of the defaults file and the savegame root
// Locate and validate an IWAD file
// Determine gamemode from the IWAD
//
// supports IWADs with custom names. Also allows the -iwad parameter to
// specify which iwad is being searched for if several exist in one dir.
// The -iwad parm may specify:
//
// 1) a specific pathname, which must exist (.wad optional)
// 2) or a directory, which must contain a standard IWAD,
// 3) or a filename, which must be found in one of the standard places:
//   a) current dir,
//   b) exe dir
//   c) $DOOMWADDIR
//   d) or $HOME
//
// jff 4/19/98 rewritten to use a more advanced search algorithm

void IdentifyVersion (void)
{
  int         i;    //jff 3/24/98 index of args on commandline
  struct stat sbuf; //jff 3/24/98 used to test save path for existence
  char *iwad;

  // get config file from same directory as executable
  // killough 10/98
  if (basedefault) (free)(basedefault);
  basedefault = M_StringJoin(D_DoomPrefDir(), DIR_SEPARATOR_S, D_DoomExeName(), ".cfg", NULL);

  // set save path to -save parm or current dir

  basesavegame = M_StringDuplicate(D_DoomPrefDir());       //jff 3/27/98 default to current dir
  if ((i=M_CheckParm("-save")) && i<myargc-1) //jff 3/24/98 if -save present
    {
      if (!stat(myargv[i+1],&sbuf) && S_ISDIR(sbuf.st_mode)) // and is a dir
      {
        if (basesavegame) (free)(basesavegame);
        basesavegame = M_StringDuplicate(myargv[i+1]);
      }
      else
        puts("Error: -save path does not exist, using current dir");  // killough 8/8/98
    }

  // locate the IWAD and determine game mode from it

  iwad = FindIWADFile();

  if (iwad && *iwad)
    {
      printf("IWAD found: %s\n",iwad); //jff 4/20/98 print only if found

      CheckIWAD(iwad);

      switch(gamemode)
        {
        case retail:
          if (gamemission == pack_chex)
            puts("Chex(R) Quest");
          else if (gamemission == pack_rekkr)
            puts("REKKR");
          else
          puts("Ultimate DOOM version");  // killough 8/8/98
          break;

        case registered:
          puts("DOOM Registered version");
          break;

        case shareware:
          puts("DOOM Shareware version");
          break;

        case commercial:

          // joel 10/16/98 Final DOOM fix
          switch (gamemission)
            {
            case pack_hacx:
              puts ("HACX: Twitch n' Kill");
              break;

            case pack_tnt:
              puts ("Final DOOM: TNT - Evilution version");
              break;

            case pack_plut:
              puts ("Final DOOM: The Plutonia Experiment version");
              break;

            case doom2:
            default:

              i = strlen(iwad);
              if (i>=10 && !strnicmp(iwad+i-10,"doom2f.wad",10))
                {
                  language=french;
                  puts("DOOM II version, French language");  // killough 8/8/98
                }
              else
                puts("DOOM II version");
              break;
            }
          // joel 10/16/88 end Final DOOM fix

        default:
          break;
        }

      if (gamemode == indetermined)
        puts("Unknown Game Version, may not work");  // killough 8/8/98

      D_AddFile(iwad);
    }
  else
    I_Error("IWAD not found\n");
}

// [FG] emulate a specific version of Doom

static struct
{
    const char *description;
    const char *cmdline;
    GameVersion_t version;
} gameversions[] = {
    {"Doom 1.9",      "1.9",      exe_doom_1_9},
    {"Ultimate Doom", "ultimate", exe_ultimate},
    {"Final Doom",    "final",    exe_final},
    {"Chex Quest",    "chex",     exe_chex},
    { NULL,           NULL,       0},
};

static void InitGameVersion(void)
{
    int i, p;

    p = M_CheckParm("-gameversion");

    if (p && p < myargc-1)
    {
        for (i=0; gameversions[i].description != NULL; ++i)
        {
            if (!strcmp(myargv[p+1], gameversions[i].cmdline))
            {
                gameversion = gameversions[i].version;
                break;
            }
        }

        if (gameversions[i].description == NULL)
        {
            printf("Supported game versions:\n");

            for (i=0; gameversions[i].description != NULL; ++i)
            {
                printf("\t%s (%s)\n", gameversions[i].cmdline,
                        gameversions[i].description);
            }

            I_Error("Unknown game version '%s'", myargv[p+1]);
        }
    }
    else
    {
        // Determine automatically

        if (gamemode == shareware || gamemode == registered ||
            (gamemode == commercial && gamemission == doom2))
        {
            // original
            gameversion = exe_doom_1_9;
        }
        else if (gamemode == retail)
        {
            if (gamemission == pack_chex)
            {
                gameversion = exe_chex;
            }
            else
            {
            gameversion = exe_ultimate;
            }
        }
        else if (gamemode == commercial)
        {
            // Final Doom: tnt or plutonia
            // Defaults to emulating the first Final Doom executable,
            // which has the crash in the demo loop; however, having
            // this as the default should mean that it plays back
            // most demos correctly.

            gameversion = exe_final;
        }
    }
}

const char* GetGameVersionCmdline(void)
{
  return gameversions[gameversion].cmdline;
}

// killough 5/3/98: old code removed

//
// Find a Response File
//

#define MAXARGVS 100

void FindResponseFile (void)
{
  int i;

  for (i = 1;i < myargc;i++)
    if (myargv[i][0] == '@')
      {
        FILE *handle;
        int  size;
        int  k;
        int  index;
        int  indexinfile;
        char *infile;
        char *file;
        char *moreargs[MAXARGVS];
        char *firstargv;

        // READ THE RESPONSE FILE INTO MEMORY

        // killough 10/98: add default .rsp extension
        char *filename = malloc(strlen(myargv[i])+5);
        AddDefaultExtension(strcpy(filename,&myargv[i][1]),".rsp");

        handle = fopen(filename,"rb");
        if (!handle)
          I_Error("No such response file!");          // killough 10/98

        printf("Found response file %s!\n",filename);
        free(filename);

        fseek(handle,0,SEEK_END);
        size = ftell(handle);
        fseek(handle,0,SEEK_SET);
        file = malloc (size);
        // [FG] check return value
        if (!fread(file,size,1,handle))
        {
          fclose(handle);
          free(file);
          return;
        }
        fclose(handle);

        // KEEP ALL CMDLINE ARGS FOLLOWING @RESPONSEFILE ARG
        for (index = 0,k = i+1; k < myargc; k++)
          moreargs[index++] = myargv[k];

        firstargv = myargv[0];
        myargv = calloc(sizeof(char *),MAXARGVS);
        myargv[0] = firstargv;

        infile = file;
        indexinfile = k = 0;
        indexinfile++;  // SKIP PAST ARGV[0] (KEEP IT)
        do
          {
            myargv[indexinfile++] = infile+k;
            while(k < size &&
                  ((*(infile+k)>= ' '+1) && (*(infile+k)<='z')))
              k++;
            *(infile+k) = 0;
            while(k < size &&
                  ((*(infile+k)<= ' ') || (*(infile+k)>'z')))
              k++;
          }
        while(k < size);

        for (k = 0;k < index;k++)
          myargv[indexinfile++] = moreargs[k];
        myargc = indexinfile;

        // DISPLAY ARGS
        printf("%d command-line args:\n",myargc-1); // killough 10/98
        for (k=1;k<myargc;k++)
          printf("%s\n",myargv[k]);
        break;
      }
}

// [FG] compose a proper command line from loose file paths passed as arguments
// to allow for loading WADs and DEHACKED patches by drag-and-drop

#if defined(_WIN32)
enum
{
    FILETYPE_UNKNOWN = 0x0,
    FILETYPE_IWAD =    0x2,
    FILETYPE_PWAD =    0x4,
    FILETYPE_DEH =     0x8,
};

static boolean D_IsIWADName(const char *name)
{
    int i;

    for (i = 0; i < arrlen(standard_iwads); i++)
    {
        if (!strcasecmp(name, standard_iwads[i].name))
        {
            return true;
        }
    }

    return false;
}

static int GuessFileType(const char *name)
{
    int ret = FILETYPE_UNKNOWN;
    const char *base;
    char *lower;
    static boolean iwad_found = false;

    base = M_BaseName(name);
    lower = M_StringDuplicate(base);
    M_ForceLowercase(lower);

    // only ever add one argument to the -iwad parameter

    if (iwad_found == false && D_IsIWADName(lower))
    {
        ret = FILETYPE_IWAD;
        iwad_found = true;
    }
    else if (M_StringEndsWith(lower, ".wad") ||
             M_StringEndsWith(lower, ".lmp"))
    {
        ret = FILETYPE_PWAD;
    }
    else if (M_StringEndsWith(lower, ".deh") ||
             M_StringEndsWith(lower, ".bex"))
    {
        ret = FILETYPE_DEH;
    }

    (free)(lower);

    return ret;
}

typedef struct
{
    char *str;
    int type, stable;
} argument_t;

static int CompareByFileType(const void *a, const void *b)
{
    const argument_t *arg_a = (const argument_t *) a;
    const argument_t *arg_b = (const argument_t *) b;

    const int ret = arg_a->type - arg_b->type;

    return ret ? ret : (arg_a->stable - arg_b->stable);
}

static void M_AddLooseFiles(void)
{
    int i, types = 0;
    char **newargv;
    argument_t *arguments;

    if (myargc < 2)
    {
        return;
    }

    // allocate space for up to three additional regular parameters

    arguments = malloc((myargc + 3) * sizeof(*arguments));
    memset(arguments, 0, (myargc + 3) * sizeof(*arguments));

    // check the command line and make sure it does not already
    // contain any regular parameters or response files
    // but only fully-qualified LFS or UNC file paths

    for (i = 1; i < myargc; i++)
    {
        char *arg = myargv[i];
        int type;

        if (strlen(arg) < 3 ||
            arg[0] == '-' ||
            arg[0] == '@' ||
            ((!isalpha(arg[0]) || arg[1] != ':' || arg[2] != '\\') &&
            (arg[0] != '\\' || arg[1] != '\\')))
        {
            free(arguments);
            return;
        }

        type = GuessFileType(arg);
        arguments[i].str = arg;
        arguments[i].type = type;
        arguments[i].stable = i;
        types |= type;
    }

    // add space for one additional regular parameter
    // for each discovered file type in the new argument list
    // and sort parameters right before their corresponding file paths

    if (types & FILETYPE_IWAD)
    {
        arguments[myargc].str = M_StringDuplicate("-iwad");
        arguments[myargc].type = FILETYPE_IWAD - 1;
        myargc++;
    }
    if (types & FILETYPE_PWAD)
    {
        arguments[myargc].str = M_StringDuplicate("-file");
        arguments[myargc].type = FILETYPE_PWAD - 1;
        myargc++;
    }
    if (types & FILETYPE_DEH)
    {
        arguments[myargc].str = M_StringDuplicate("-deh");
        arguments[myargc].type = FILETYPE_DEH - 1;
        myargc++;
    }

    newargv = malloc(myargc * sizeof(*newargv));

    // sort the argument list by file type, except for the zeroth argument
    // which is the executable invocation itself

    SDL_qsort(arguments + 1, myargc - 1, sizeof(*arguments), CompareByFileType);

    newargv[0] = myargv[0];

    for (i = 1; i < myargc; i++)
    {
        newargv[i] = arguments[i].str;
    }

    free(arguments);

    myargv = newargv;
}
#endif

// killough 10/98: moved code to separate function

static void D_ProcessDehCommandLine(void)
{
  // ty 03/09/98 do dehacked stuff
  // Note: do this before any other since it is expected by
  // the deh patch author that this is actually part of the EXE itself
  // Using -deh in BOOM, others use -dehacked.
  // Ty 03/18/98 also allow .bex extension.  .bex overrides if both exist.
  // killough 11/98: also allow -bex

  int p = M_CheckParm ("-deh");
  if (p || (p = M_CheckParm("-bex")))
    {
      // the parms after p are deh/bex file names,
      // until end of parms or another - preceded parm
      // Ty 04/11/98 - Allow multiple -deh files in a row
      // killough 11/98: allow multiple -deh parameters

      boolean deh = true;
      while (++p < myargc)
        if (*myargv[p] == '-')
          deh = !strcasecmp(myargv[p],"-deh") || !strcasecmp(myargv[p],"-bex");
        else
          if (deh)
            {
              char *probe;
              char *file = (malloc)(strlen(myargv[p]) + 5);      // killough
              AddDefaultExtension(strcpy(file, myargv[p]), ".bex");
              probe = D_TryFindWADByName(file);
              if (access(probe, F_OK))  // nope
                {
                  (free)(probe);
                  AddDefaultExtension(strcpy(file, myargv[p]), ".deh");
                  probe = D_TryFindWADByName(file);
                  if (access(probe, F_OK))  // still nope
                  {
                    (free)(probe);
                    I_Error("Cannot find .deh or .bex file named %s",
                            myargv[p]);
                  }
                }
              // during the beta we have debug output to dehout.txt
              // (apparently, this was never removed after Boom beta-killough)
              ProcessDehFile(probe, D_dehout(), 0);  // killough 10/98
              (free)(probe);
              (free)(file);
            }
    }
  // ty 03/09/98 end of do dehacked stuff
}

// Load all WAD files from the given directory.

static void AutoLoadWADs(const char *path)
{
    glob_t *glob;
    const char *filename;

    glob = I_StartMultiGlob(path, GLOB_FLAG_NOCASE|GLOB_FLAG_SORTED,
                            "*.wad", "*.lmp", NULL);
    for (;;)
    {
        filename = I_NextGlob(glob);
        if (filename == NULL)
        {
            break;
        }
        D_AddFile(filename);
    }

    I_EndGlob(glob);
}

// auto-loading of .wad files.

static void D_AutoloadIWadDir()
{
  char **base;

  for (base = autoload_paths; base && *base; base++)
  {
    char *autoload_dir;

    // common auto-loaded files for all Doom flavors
    if (gamemission < pack_chex)
    {
      autoload_dir = GetAutoloadDir(*base, "doom-all", true);
      AutoLoadWADs(autoload_dir);
      (free)(autoload_dir);
    }

    // auto-loaded files per IWAD
    autoload_dir = GetAutoloadDir(*base, M_BaseName(wadfiles[0]), true);
    AutoLoadWADs(autoload_dir);
    (free)(autoload_dir);
  }
}

static void D_AutoloadPWadDir()
{
  int p = M_CheckParm("-file");
  if (p)
  {
    while (++p != myargc && myargv[p][0] != '-')
    {
      char **base;

      for (base = autoload_paths; base && *base; base++)
      {
        char *autoload_dir;
        autoload_dir = GetAutoloadDir(*base, M_BaseName(myargv[p]), false);
        AutoLoadWADs(autoload_dir);
        (free)(autoload_dir);
      }
    }
  }
}

// killough 10/98: support preloaded wads

static void D_ProcessWadPreincludes(void)
{
  if (!M_CheckParm ("-noload"))
    {
      int i;
      char *s;
      for (i=0; i<MAXLOADFILES; i++)
        if ((s=wad_files[i]))
          {
            while (isspace(*s))
              s++;
            if (*s)
              {
                char *file = (malloc)(strlen(s) + 5);
                AddDefaultExtension(strcpy(file, s), ".wad");
                if (!access(file, R_OK))
                  D_AddFile(file);
                else
                  printf("\nWarning: could not open %s\n", file);
                (free)(file);
              }
          }
    }
}

// Load all dehacked patches from the given directory.

static void AutoLoadPatches(const char *path)
{
    const char *filename;
    glob_t *glob;

    glob = I_StartMultiGlob(path, GLOB_FLAG_NOCASE|GLOB_FLAG_SORTED,
                            "*.deh", "*.bex", NULL);
    for (;;)
    {
        filename = I_NextGlob(glob);
        if (filename == NULL)
        {
            break;
        }
        ProcessDehFile(filename, D_dehout(), 0);
    }

    I_EndGlob(glob);
}

// auto-loading of .deh files.

static void D_AutoloadDehDir()
{
  char **base;

  for (base = autoload_paths; base && *base; base++)
  {
    char *autoload_dir;

    // common auto-loaded files for all Doom flavors
    if (gamemission < pack_chex)
    {
      autoload_dir = GetAutoloadDir(*base, "doom-all", true);
      AutoLoadPatches(autoload_dir);
      (free)(autoload_dir);
    }

    // auto-loaded files per IWAD
    autoload_dir = GetAutoloadDir(*base, M_BaseName(wadfiles[0]), true);
    AutoLoadPatches(autoload_dir);
    (free)(autoload_dir);
  }
}

static void D_AutoloadPWadDehDir()
{
  int p = M_CheckParm("-file");
  if (p)
  {
    while (++p != myargc && myargv[p][0] != '-')
    {
      char **base;

      for (base = autoload_paths; base && *base; base++)
      {
        char *autoload_dir;
        autoload_dir = GetAutoloadDir(*base, M_BaseName(myargv[p]), false);
        AutoLoadPatches(autoload_dir);
        (free)(autoload_dir);
      }
    }
  }
}

// killough 10/98: support preloaded deh/bex files

static void D_ProcessDehPreincludes(void)
{
  if (!M_CheckParm ("-noload"))
    {
      int i;
      char *s;
      for (i=0; i<MAXLOADFILES; i++)
        if ((s=deh_files[i]))
          {
            while (isspace(*s))
              s++;
            if (*s)
              {
                char *file = (malloc)(strlen(s) + 5);
                AddDefaultExtension(strcpy(file, s), ".bex");
                if (!access(file, R_OK))
                  ProcessDehFile(file, D_dehout(), 0);
                else
                  {
                    AddDefaultExtension(strcpy(file, s), ".deh");
                    if (!access(file, R_OK))
                      ProcessDehFile(file, D_dehout(), 0);
                    else
                      printf("\nWarning: could not open %s .deh or .bex\n", s);
                  }
                (free)(file);
              }
          }
    }
}

// killough 10/98: support .deh from wads
//
// A lump named DEHACKED is treated as plaintext of a .deh file embedded in
// a wad (more portable than reading/writing info.c data directly in a wad).
//
// If there are multiple instances of "DEHACKED", we process each, in first
// to last order (we must reverse the order since they will be stored in
// last to first order in the chain). Passing NULL as first argument to
// ProcessDehFile() indicates that the data comes from the lump number
// indicated by the third argument, instead of from a file.

static void D_ProcessDehInWad(int i, boolean in_iwad)
{
  // [FG] avoid loading DEHACKED lumps embedded into WAD files
  if (M_CheckParm("-nodehlump"))
  {
    return;
  }

  if (i >= 0)
    {
      D_ProcessDehInWad(lumpinfo[i].next, in_iwad);
      if (!strncasecmp(lumpinfo[i].name, "dehacked", 8) &&
          lumpinfo[i].namespace == ns_global &&
          (in_iwad ? W_IsIWADLump(i) : !W_IsIWADLump(i)))
        ProcessDehFile(NULL, D_dehout(), i);
    }
}

#define D_ProcessDehInWads() D_ProcessDehInWad(lumpinfo[W_LumpNameHash \
                                                       ("dehacked") % (unsigned) numlumps].index, false);

#define D_ProcessDehInIWad() D_ProcessDehInWad(lumpinfo[W_LumpNameHash \
                                                       ("dehacked") % (unsigned) numlumps].index, true);

// Process multiple UMAPINFO files

static void D_ProcessUMInWad(int i)
{
  if (i >= 0)
    {
      D_ProcessUMInWad(lumpinfo[i].next);
      if (!strncasecmp(lumpinfo[i].name, "umapinfo", 8) &&
          lumpinfo[i].namespace == ns_global)
        {
          U_ParseMapInfo(false, (const char *)W_CacheLumpNum(i, PU_CACHE), W_LumpLength(i));
        }
    }
}

#define D_ProcessUMInWads() D_ProcessUMInWad(lumpinfo[W_LumpNameHash \
                                                       ("umapinfo") % (unsigned) numlumps].index);

// Process multiple UMAPDEF files

static void D_ProcessDefaultsInWad(int i)
{
  if (i >= 0)
    {
      D_ProcessDefaultsInWad(lumpinfo[i].next);
      if (!strncasecmp(lumpinfo[i].name, "umapdef", 7) &&
          lumpinfo[i].namespace == ns_global)
        {
          U_ParseMapInfo(true, (const char *)W_CacheLumpNum(i, PU_CACHE), W_LumpLength(i));
        }
    }
}

#define D_ProcessDefaultsInWads() D_ProcessDefaultsInWad(lumpinfo[W_LumpNameHash \
                                                       ("umapdef") % (unsigned) numlumps].index);

// mbf21: don't want to reorganize info.c structure for a few tweaks...

static void D_InitTables(void)
{
  int i;
  for (i = 0; i < num_mobj_types; ++i)
  {
    mobjinfo[i].flags2           = 0;
    mobjinfo[i].infighting_group = IG_DEFAULT;
    mobjinfo[i].projectile_group = PG_DEFAULT;
    mobjinfo[i].splash_group     = SG_DEFAULT;
    mobjinfo[i].ripsound         = sfx_None;
    mobjinfo[i].altspeed         = NO_ALTSPEED;
    mobjinfo[i].meleerange       = MELEERANGE;
    // [Woof!]
    mobjinfo[i].bloodcolor       = 0; // Normal
    // DEHEXTRA
    mobjinfo[i].droppeditem      = MT_NULL;
  }

  mobjinfo[MT_VILE].flags2    = MF2_SHORTMRANGE | MF2_DMGIGNORED | MF2_NOTHRESHOLD;
  mobjinfo[MT_CYBORG].flags2  = MF2_NORADIUSDMG | MF2_HIGHERMPROB | MF2_RANGEHALF |
                                MF2_FULLVOLSOUNDS | MF2_E2M8BOSS | MF2_E4M6BOSS;
  mobjinfo[MT_SPIDER].flags2  = MF2_NORADIUSDMG | MF2_RANGEHALF | MF2_FULLVOLSOUNDS |
                                MF2_E3M8BOSS | MF2_E4M8BOSS;
  mobjinfo[MT_SKULL].flags2   = MF2_RANGEHALF;
  mobjinfo[MT_FATSO].flags2   = MF2_MAP07BOSS1;
  mobjinfo[MT_BABY].flags2    = MF2_MAP07BOSS2;
  mobjinfo[MT_BRUISER].flags2 = MF2_E1M8BOSS;
  mobjinfo[MT_UNDEAD].flags2  = MF2_LONGMELEE | MF2_RANGEHALF;

  mobjinfo[MT_BRUISER].projectile_group = PG_BARON;
  mobjinfo[MT_KNIGHT].projectile_group = PG_BARON;

  mobjinfo[MT_BRUISERSHOT].altspeed = 20 * FRACUNIT;
  mobjinfo[MT_HEADSHOT].altspeed = 20 * FRACUNIT;
  mobjinfo[MT_TROOPSHOT].altspeed = 20 * FRACUNIT;

  // DEHEXTRA
  mobjinfo[MT_WOLFSS].droppeditem = MT_CLIP;
  mobjinfo[MT_POSSESSED].droppeditem = MT_CLIP;
  mobjinfo[MT_SHOTGUY].droppeditem = MT_SHOTGUN;
  mobjinfo[MT_CHAINGUY].droppeditem = MT_CHAINGUN;

  // [crispy] randomly mirrored death animations
  for (i = MT_PLAYER; i <= MT_KEEN; ++i)
  {
    switch (i)
    {
      case MT_FIRE:
      case MT_TRACER:
      case MT_SMOKE:
      case MT_FATSHOT:
      case MT_BRUISERSHOT:
      case MT_CYBORG:
        continue;
    }
    mobjinfo[i].flags2 |= MF2_FLIPPABLE;
  }

  mobjinfo[MT_PUFF].flags2 |= MF2_FLIPPABLE;
  mobjinfo[MT_BLOOD].flags2 |= MF2_FLIPPABLE;

  for (i = MT_MISC61; i <= MT_MISC69; ++i)
     mobjinfo[i].flags2 |= MF2_FLIPPABLE;

  mobjinfo[MT_DOGS].flags2 |= MF2_FLIPPABLE;

  for (i = S_SARG_RUN1; i <= S_SARG_PAIN2; ++i)
    states[i].flags |= STATEF_SKILL5FAST;
}

// [FG] fast-forward demo to the desired map
int demowarp = -1;

//
// D_DoomMain
//

void D_DoomMain(void)
{
  int p, slot;

  setbuf(stdout,NULL);

  dsdh_InitTables();

#if defined(_WIN32)
    // [FG] compose a proper command line from loose file paths passed as arguments
    // to allow for loading WADs and DEHACKED patches by drag-and-drop
    M_AddLooseFiles();
#endif

  FindResponseFile();         // Append response file arguments to command-line

  // killough 10/98: set default savename based on executable's name
  sprintf(savegamename = malloc(16), "%.4ssav", D_DoomExeName());

  IdentifyVersion();

  // [FG] emulate a specific version of Doom
  InitGameVersion();

  D_InitTables();

  modifiedgame = false;

  // killough 7/19/98: beta emulation option
  beta_emulation = !!M_CheckParm("-beta");

  if (beta_emulation)
    { // killough 10/98: beta lost soul has different behavior frames
      mobjinfo[MT_SKULL].spawnstate   = S_BSKUL_STND;
      mobjinfo[MT_SKULL].seestate     = S_BSKUL_RUN1;
      mobjinfo[MT_SKULL].painstate    = S_BSKUL_PAIN1;
      mobjinfo[MT_SKULL].missilestate = S_BSKUL_ATK1;
      mobjinfo[MT_SKULL].deathstate   = S_BSKUL_DIE1;
      mobjinfo[MT_SKULL].damage       = 1;
    }
#ifdef MBF_STRICT
  // This code causes MT_SCEPTRE and MT_BIBLE to not spawn on the map,
  // which causes desync in Eviternity.wad demos.
  else
    mobjinfo[MT_SCEPTRE].doomednum = mobjinfo[MT_BIBLE].doomednum = -1;
#endif

  // jff 1/24/98 set both working and command line value of play parms
  nomonsters = clnomonsters = M_CheckParm ("-nomonsters");
  respawnparm = clrespawnparm = M_CheckParm ("-respawn");
  fastparm = clfastparm = M_CheckParm ("-fast");
  // jff 1/24/98 end of set to both working and command line value

  pistolstart = M_CheckParm ("-pistolstart");

  devparm = M_CheckParm ("-devparm");

  if (M_CheckParm ("-altdeath"))
    deathmatch = 2;
  else
    if (M_CheckParm ("-deathmatch"))
      deathmatch = 1;

  switch ( gamemode )
    {
    case retail:
      sprintf (title,
               "                         "
               "The Ultimate DOOM Startup v%i.%02i"
               "                           ",
               MBFVERSION/100,MBFVERSION%100);
      break;
    case shareware:
      sprintf (title,
               "                            "
               "DOOM Shareware Startup v%i.%02i"
               "                           ",
               MBFVERSION/100,MBFVERSION%100);
      break;

    case registered:
      sprintf (title,
               "                            "
               "DOOM Registered Startup v%i.%02i"
               "                           ",
               MBFVERSION/100,MBFVERSION%100);
      break;

    case commercial:
      switch (gamemission)      // joel 10/16/98 Final DOOM fix
        {
        case pack_plut:
          sprintf (title,
                   "                   "
                   "DOOM 2: Plutonia Experiment v%i.%02i"
                   "                           ",
                   MBFVERSION/100,MBFVERSION%100);
          break;

        case pack_tnt:
          sprintf (title,
                   "                     "
                   "DOOM 2: TNT - Evilution v%i.%02i"
                   "                           ",
                   MBFVERSION/100,MBFVERSION%100);
          break;

        case doom2:
        default:

          sprintf (title,
                   "                         "
                   "DOOM 2: Hell on Earth v%i.%02i"
                   "                           ",
                   MBFVERSION/100,MBFVERSION%100);

          break;
        }
      break;
      // joel 10/16/98 end Final DOOM fix

    default:
      sprintf (title,
               "                     "
               "Public DOOM - v%i.%i"
               "                           ",
               MBFVERSION/100,MBFVERSION%100);
      break;
    }

  printf("%s\nBuilt on %s\n", title, version_date);    // killough 2/1/98

  if (devparm)
    printf(D_DEVSTR);

#ifdef _WIN32
  if (M_CheckParm("-cdrom"))
    {
      printf(D_CDROM);
      mkdir("c:\\doomdata");

      // killough 10/98:
      if (basedefault) (free)(basedefault);
      basedefault = M_StringJoin("c:\\doomdata\\", D_DoomExeName(), ".cfg", NULL);
    }
#endif

  // turbo option
  if ((p=M_CheckParm ("-turbo")))
    {
      int scale = 200;
      extern int forwardmove[2];
      extern int sidemove[2];

      if (p<myargc-1)
        scale = atoi(myargv[p+1]);
      if (scale < 10)
        scale = 10;
      if (scale > 400)
        scale = 400;
      printf ("turbo scale: %i%%\n",scale);
      forwardmove[0] = forwardmove[0]*scale/100;
      forwardmove[1] = forwardmove[1]*scale/100;
      sidemove[0] = sidemove[0]*scale/100;
      sidemove[1] = sidemove[1]*scale/100;
    }

  if (beta_emulation)
    {
      D_AddFile("betagrph.wad");
    }

  // add wad files from autoload IWAD directories before wads from -file parameter

  PrepareAutoloadPaths();
  D_AutoloadIWadDir();

  // add any files specified on the command line with -file wadfile
  // to the wad list

  // killough 1/31/98, 5/2/98: reload hack removed, -wart same as -warp now.

  if ((p = M_CheckParm ("-file")))
    {
      // the parms after p are wadfile/lump names,
      // until end of parms or another - preceded parm
      // killough 11/98: allow multiple -file parameters

      boolean file = modifiedgame = true;            // homebrew levels
      while (++p < myargc)
        if (*myargv[p] == '-')
          file = !strcasecmp(myargv[p],"-file");
        else
          if (file)
            D_AddFile(myargv[p]);
    }

  // add wad files from autoload PWAD directories

  D_AutoloadPWadDir();

  if (!(p = M_CheckParm("-playdemo")) || p >= myargc-1)    // killough
  {
    if ((p = M_CheckParm ("-fastdemo")) && p < myargc-1)   // killough
      fastdemo = true;             // run at fastest speed possible
    else
      p = M_CheckParm ("-timedemo");
  }

  if (p && p < myargc-1)
    {
      char *file = (malloc)(strlen(myargv[p+1]) + 5);
      strcpy(file,myargv[p+1]);
      AddDefaultExtension(file,".lmp");     // killough
      D_AddFile(file);
      printf("Playing demo %s\n",file);
      (free)(file);
    }

  // get skill / episode / map from parms

  startskill = sk_none; // jff 3/24/98 was sk_medium, just note not picked
  startepisode = 1;
  startmap = 1;
  autostart = false;

  if ((p = M_CheckParm ("-skill")) && p < myargc-1)
    {
      startskill = myargv[p+1][0]-'1';
      autostart = true;
    }

  if ((p = M_CheckParm ("-episode")) && p < myargc-1)
    {
      startepisode = myargv[p+1][0]-'0';
      startmap = 1;
      autostart = true;
    }

  if ((p = M_CheckParm ("-timer")) && p < myargc-1 && deathmatch)
    {
      int time = atoi(myargv[p+1]);
      timelimit = time;
      printf("Levels will end after %d minute%s.\n", time, time>1 ? "s" : "");
    }

  if ((p = M_CheckParm ("-avg")) && p < myargc-1 && deathmatch)
    {
    timelimit = 20;
    puts("Austin Virtual Gaming: Levels will end after 20 minutes");
    }

  if (((p = M_CheckParm ("-warp")) ||      // killough 5/2/98
       (p = M_CheckParm ("-wart"))) && p < myargc-1)
  {
    if (gamemode == commercial)
      {
        startmap = atoi(myargv[p+1]);
        autostart = true;
      }
    else    // 1/25/98 killough: fix -warp xxx from crashing Doom 1 / UD
      // [crispy] only if second argument is not another option
      if (p < myargc-2 && myargv[p+2][0] != '-')
        {
          startepisode = atoi(myargv[++p]);
          startmap = atoi(myargv[p+1]);
          autostart = true;
        }
      // [crispy] allow second digit without space in between for Doom 1
      else
        {
          int em = atoi(myargv[++p]);
          startepisode = em / 10;
          startmap = em % 10;
          autostart = true;
        }
    // [FG] fast-forward demo to the desired map
    demowarp = startmap;
  }

  //jff 1/22/98 add command line parms to disable sound and music
  {
    int nosound = M_CheckParm("-nosound");
    nomusicparm = nosound || M_CheckParm("-nomusic");
    nosfxparm   = nosound || M_CheckParm("-nosfx");
  }
  //jff end of sound/music command line parms

  // killough 3/2/98: allow -nodraw -noblit generally
  nodrawers = M_CheckParm ("-nodraw");
  noblit = M_CheckParm ("-noblit");

  // jff 4/21/98 allow writing predefined lumps out as a wad
  if ((p = M_CheckParm("-dumplumps")) && p < myargc-1)
    WritePredefinedLumpWad(myargv[p+1]);

  puts("M_LoadDefaults: Load system defaults.");
  M_LoadDefaults();              // load before initing other systems

  bodyquesize = default_bodyquesize; // killough 10/98

  // 1/18/98 killough: Z_Init call moved to i_main.c

  // init subsystems
  puts("V_Init: allocate screens.");    // killough 11/98: moved down to here
  V_Init();

  D_ProcessWadPreincludes(); // killough 10/98: add preincluded wads at the end

  D_AddFile(NULL);           // killough 11/98

  puts("W_Init: Init WADfiles.");
  W_InitMultipleFiles(wadfiles);

  // Check for wolf levels
  haswolflevels = (W_CheckNumForName("map31") >= 0);

  // Moved after WAD initialization because we are checking the COMPLVL lump
  G_ReloadDefaults();    // killough 3/4/98: set defaults just loaded.
  // jff 3/24/98 this sets startskill if it was -1

  putchar('\n');     // killough 3/6/98: add a newline, by popular demand :)

  // process deh in IWAD
  D_ProcessDehInIWad();

  // process .deh files specified on the command line with -deh or -bex.
  D_ProcessDehCommandLine();

  // process deh in wads and .deh files from autoload directory
  // before deh in wads from -file parameter

  D_AutoloadDehDir();

  D_ProcessDehInWads();      // killough 10/98: now process all deh in wads

  // process .deh files from PWADs autoload directories

  D_AutoloadPWadDehDir();

  D_ProcessDehPreincludes(); // killough 10/98: process preincluded .deh files

  PostProcessDeh();

  // Check for -file in shareware
  if (modifiedgame)
    {
      // These are the lumps that will be checked in IWAD,
      // if any one is not present, execution will be aborted.
      static const char name[23][8]= {
        "e2m1","e2m2","e2m3","e2m4","e2m5","e2m6","e2m7","e2m8","e2m9",
        "e3m1","e3m3","e3m3","e3m4","e3m5","e3m6","e3m7","e3m8","e3m9",
        "dphoof","bfgga0","heada1","cybra1","spida1d1" };
      int i;

      if (gamemode == shareware)
        I_Error("\nYou cannot -file with the shareware version. Register!");

      // Check for fake IWAD with right name,
      // but w/o all the lumps of the registered version.
      if (gamemode == registered)
        for (i = 0;i < 23; i++)
          if (W_CheckNumForName(name[i])<0 &&
              (W_CheckNumForName)(name[i],ns_sprites)<0) // killough 4/18/98
            I_Error("\nThis is not the registered version.");
    }

  D_ProcessDefaultsInWads();

  if (!M_CheckParm("-nomapinfo"))
  {
    D_ProcessUMInWads();
  }

  V_InitColorTranslation(); //jff 4/24/98 load color translation lumps

  // killough 2/22/98: copyright / "modified game" / SPA banners removed

  // Ty 04/08/98 - Add 5 lines of misc. data, only if nonblank
  // The expectation is that these will be set in a .bex file
  if (*startup1) puts(startup1);
  if (*startup2) puts(startup2);
  if (*startup3) puts(startup3);
  if (*startup4) puts(startup4);
  if (*startup5) puts(startup5);
  // End new startup strings

  p = M_CheckParmWithArgs("-loadgame", 1);
  if (p)
  {
    startloadgame = atoi(myargv[p+1]);
  }
  else
  {
    // Not loading a game
    startloadgame = -1;
  }

  puts("M_Init: Init miscellaneous info.");
  M_Init();

  printf("R_Init: Init DOOM refresh daemon - ");
  R_Init();

  puts("\nP_Init: Init Playloop state.");
  P_Init();

  puts("I_Init: Setting up machine state.");
  I_Init();

  puts("NET_Init: Init network subsystem.");
  NET_Init();

  // Initial netgame startup. Connect to server etc.
  D_ConnectNetGame();

  puts("D_CheckNetGame: Checking network game status.");
  D_CheckNetGame();

  puts("S_Init: Setting up sound.");
  S_Init(snd_SfxVolume /* *8 */, snd_MusicVolume /* *8*/ );

  puts("HU_Init: Setting up heads up display.");
  HU_Init();

  puts("ST_Init: Init status bar.");
  ST_Init();

  idmusnum = -1; //jff 3/17/98 insure idmus number is blank

  // check for a driver that wants intermission stats
  // [FG] replace with -statdump implementation from Chocolate Doom
  if ((p = M_CheckParm ("-statdump")) && p<myargc-1)
    {
      I_AtExit(StatDump, true);
      puts("External statistics registered.");
    }

  if (M_ParmExists("-coop_spawns"))
    {
      coop_spawns = true;
    }

  // start the apropriate game based on parms

  // killough 12/98: 
  // Support -loadgame with -record and reimplement -recordfrom.

  if ((slot = M_CheckParm("-recordfrom")) && (p = slot+2) < myargc)
    G_RecordDemo(myargv[p]);
  else
    {
      slot = M_CheckParm("-loadgame");
      if ((p = M_CheckParm("-record")) && ++p < myargc)
	{
	  autostart = true;
	  G_RecordDemo(myargv[p]);
	}
    }

  if ((p = M_CheckParm ("-fastdemo")) && ++p < myargc)
    {                                 // killough
      fastdemo = true;                // run at fastest speed possible
      timingdemo = true;              // show stats after quit
      G_DeferedPlayDemo(myargv[p]);
      singledemo = true;              // quit after one demo
    }
  else
    if ((p = M_CheckParm("-timedemo")) && ++p < myargc)
      {
	singletics = true;
	timingdemo = true;            // show stats after quit
	G_DeferedPlayDemo(myargv[p]);
	singledemo = true;            // quit after one demo
      }
    else
      if ((p = M_CheckParm("-playdemo")) && ++p < myargc)
	{
	  G_DeferedPlayDemo(myargv[p]);
	  singledemo = true;          // quit after one demo
	}
	else
	  // [FG] no demo playback
	  demowarp = -1;

  if (slot && ++slot < myargc)
  {
    startloadgame = atoi(myargv[slot]);
  }

  // [FG] init graphics (WIDESCREENDELTA) before HUD widgets
  I_InitGraphics();

  if (startloadgame >= 0)
  {
    char *file;
    file = G_SaveGameName(startloadgame);
    G_LoadGame(file, startloadgame, true); // killough 5/15/98: add command flag
    (free)(file);
  }
  else
    if (!singledemo)                    // killough 12/98
    {
      if (autostart || netgame)
	{
	  G_InitNew(startskill, startepisode, startmap);
	  if (demorecording)
	    G_BeginRecording();
	}
      else
	D_StartTitle();                 // start up intro loop
    }

  // killough 12/98: inlined D_DoomLoop

  if (M_CheckParm ("-debugfile"))
    {
      char filename[20];
      sprintf(filename,"debug%i.txt",consoleplayer);
      printf("debug output to: %s\n",filename);
      debugfile = fopen(filename,"w");
    }

  main_loop_started = true;

  D_StartGameLoop();

  for (;;)
    {
      // frame syncronous IO operations
      I_StartFrame ();

      TryRunTics (); // will run at least one tic

      // killough 3/16/98: change consoleplayer to displayplayer
      S_UpdateSounds(players[displayplayer].mo);// move positional sounds

      // Update display, next frame, with current state.
      D_Display();

      // Sound mixing for the buffer is snychronous.
      I_UpdateSound();

      // Synchronous sound output is explicitly called.
      // Update sound output.
      I_SubmitSound();
    }
}

//----------------------------------------------------------------------------
//
// $Log: d_main.c,v $
// Revision 1.47  1998/05/16  09:16:51  killough
// Make loadgame checksum friendlier
//
// Revision 1.46  1998/05/12  10:32:42  jim
// remove LEESFIXES from d_main
//
// Revision 1.45  1998/05/06  15:15:46  jim
// Documented IWAD routines
//
// Revision 1.44  1998/05/03  22:26:31  killough
// beautification, declarations, headers
//
// Revision 1.43  1998/04/24  08:08:13  jim
// Make text translate tables lumps
//
// Revision 1.42  1998/04/21  23:46:01  jim
// Predefined lump dumper option
//
// Revision 1.39  1998/04/20  11:06:42  jim
// Fixed print of IWAD found
//
// Revision 1.37  1998/04/19  01:12:19  killough
// Fix registered check to work with new lump namespaces
//
// Revision 1.36  1998/04/16  18:12:50  jim
// Fixed leak
//
// Revision 1.35  1998/04/14  08:14:18  killough
// Remove obsolete adaptive_gametics code
//
// Revision 1.34  1998/04/12  22:54:41  phares
// Remaining 3 Setup screens
//
// Revision 1.33  1998/04/11  14:49:15  thldrmn
// Allow multiple deh/bex files
//
// Revision 1.32  1998/04/10  06:31:50  killough
// Add adaptive gametic timer
//
// Revision 1.31  1998/04/09  09:18:17  thldrmn
// Added generic startup strings for BEX use
//
// Revision 1.30  1998/04/06  04:52:29  killough
// Allow demo_insurance=2, fix fps regression wrt redrawsbar
//
// Revision 1.29  1998/03/31  01:08:11  phares
// Initial Setup screens and Extended HELP screens
//
// Revision 1.28  1998/03/28  15:49:37  jim
// Fixed merge glitches in d_main.c and g_game.c
//
// Revision 1.27  1998/03/27  21:26:16  jim
// Default save dir offically . now
//
// Revision 1.26  1998/03/25  18:14:21  jim
// Fixed duplicate IWAD search in .
//
// Revision 1.25  1998/03/24  16:16:00  jim
// Fixed looking for wads message
//
// Revision 1.23  1998/03/24  03:16:51  jim
// added -iwad and -save parms to command line
//
// Revision 1.22  1998/03/23  03:07:44  killough
// Use G_SaveGameName, fix some remaining default.cfg's
//
// Revision 1.21  1998/03/18  23:13:54  jim
// Deh text additions
//
// Revision 1.19  1998/03/16  12:27:44  killough
// Remember savegame slot when loading
//
// Revision 1.18  1998/03/10  07:14:58  jim
// Initial DEH support added, minus text
//
// Revision 1.17  1998/03/09  07:07:45  killough
// print newline after wad files
//
// Revision 1.16  1998/03/04  08:12:05  killough
// Correctly set defaults before recording demos
//
// Revision 1.15  1998/03/02  11:24:25  killough
// make -nodraw -noblit work generally, fix ENDOOM
//
// Revision 1.14  1998/02/23  04:13:55  killough
// My own fix for m_misc.c warning, plus lots more (Rand's can wait)
//
// Revision 1.11  1998/02/20  21:56:41  phares
// Preliminarey sprite translucency
//
// Revision 1.10  1998/02/20  00:09:00  killough
// change iwad search path order
//
// Revision 1.9  1998/02/17  06:09:35  killough
// Cache D_DoomExeDir and support basesavegame
//
// Revision 1.8  1998/02/02  13:20:03  killough
// Ultimate Doom, -fastdemo -nodraw -noblit support, default_compatibility
//
// Revision 1.7  1998/01/30  18:48:15  phares
// Changed textspeed and textwait to functions
//
// Revision 1.6  1998/01/30  16:08:59  phares
// Faster end-mission text display
//
// Revision 1.5  1998/01/26  19:23:04  phares
// First rev with no ^Ms
//
// Revision 1.4  1998/01/26  05:40:12  killough
// Fix Doom 1 crashes on -warp with too few args
//
// Revision 1.3  1998/01/24  21:03:04  jim
// Fixed disappearence of nomonsters, respawn, or fast mode after demo play or IDCLEV
//
// Revision 1.1.1.1  1998/01/19  14:02:53  rand
// Lee's Jan 19 sources
//
//----------------------------------------------------------------------------
