#include "global.h"
/*
-----------------------------------------------------------------------------
 File: StepMania.cpp

 Desc: Entry point for program.

 Copyright (c) 2001-2002 by the person(s) listed below.  All rights reserved.
	Chris Danford
-----------------------------------------------------------------------------
*/

#include "StepMania.h"

//
// Rage global classes
//
#include "RageLog.h"
#include "RageTextureManager.h"
#include "RageSoundManager.h"
#include "RageInput.h"
#include "RageTimer.h"
#include "RageException.h"
#include "RageMath.h"
#include "RageDisplay.h"

#include "arch/arch.h"
#include "arch/LoadingWindow/LoadingWindow.h"
#include "arch/ErrorDialog/ErrorDialog.h"
#include "time.h"

#include "SDL_utils.h"

//
// StepMania global classes
//
#include "ThemeManager.h"
#include "NoteSkinManager.h"
#include "PrefsManager.h"
#include "SongManager.h"
#include "GameState.h"
#include "AnnouncerManager.h"
#include "ScreenManager.h"
#include "GameManager.h"
#include "FontManager.h"
#include "InputFilter.h"
#include "InputMapper.h"
#include "InputQueue.h"
#include "SongCacheIndex.h"

/* This is also a global class; we own it. */
ArchHooks *HOOKS = NULL;


#ifdef DEBUG
#pragma comment(lib, "SDL-1.2.5/lib/SDLmaind.lib")
#else
#pragma comment(lib, "SDL-1.2.5/lib/SDLmain.lib")
#endif

#ifdef WIN32
HWND g_hWndMain = NULL;
#endif

static bool g_bHasFocus = true;
static bool g_bQuitting = false;

static void ChangeToDirOfExecutable(const char *argv0)
{
	/* Make sure the current directory is the root program directory
	 * We probably shouldn't do this; rather, we should know where things
	 * are and use paths as needed, so we don't depend on the binary being
	 * in the same place as "Songs" ... */
	if( !DoesFileExist("Songs") )
	{
		// strip off executable name
		CString dir = argv0;
		unsigned n = dir.find_last_of("/\\");
		if (n != dir.npos) dir.erase(n);

		chdir( dir.c_str() );
	}
}

void ApplyGraphicOptions()
{ 
	bool bNeedReload = false;

	bNeedReload |= DISPLAY->SetVideoMode( 
		PREFSMAN->m_bWindowed, 
		PREFSMAN->m_iDisplayWidth, 
		PREFSMAN->m_iDisplayHeight, 
		PREFSMAN->m_iDisplayColorDepth, 
		PREFSMAN->m_iRefreshRate,
		PREFSMAN->m_bVsync,
		THEME->GetMetric("Common","WindowTitle"),
		THEME->GetPathToG("Common window icon") );
	bNeedReload |= TEXTUREMAN->SetPrefs( 
		PREFSMAN->m_iTextureColorDepth, 
		PREFSMAN->m_bDelayedTextureDelete, 
		PREFSMAN->m_iMaxTextureResolution );

	if( bNeedReload )
		TEXTUREMAN->ReloadAll();

	SCREENMAN->SystemMessage( ssprintf("%s %dx%d %d color %d texture %dHz %s",
		PREFSMAN->m_bWindowed ? "Windowed" : "Fullscreen",
		PREFSMAN->m_iDisplayWidth, 
		PREFSMAN->m_iDisplayHeight, 
		PREFSMAN->m_iDisplayColorDepth, 
		PREFSMAN->m_iTextureColorDepth, 
		PREFSMAN->m_iRefreshRate,
		PREFSMAN->m_bVsync ? "Vsync" : "NoSync" ) );
}

void ExitGame()
{
	g_bQuitting = true;
}

void ResetGame()
{
	GAMESTATE->Reset();
	PREFSMAN->ReadGamePrefsFromDisk();
	INPUTMAPPER->ReadMappingsFromDisk();

	/*
	GameState::Reset() will switch the NoteSkin
	for( int p=0; p<NUM_PLAYERS; p++ )
	{
		PlayerNumber pn = (PlayerNumber)p;
		if( !NOTESKIN->DoesNoteSkinExist( NOTESKIN->GetCurNoteSkinName(pn) ) )
		{
			CStringArray asNoteSkinNames;
			NOTESKIN->GetNoteSkinNames( asNoteSkinNames );
			NOTESKIN->SwitchNoteSkin( pn, asNoteSkinNames[0] );
		}
	}
	*/
	if( !THEME->DoesThemeExist( THEME->GetCurThemeName() ) )
	{
		CString sGameName = GAMESTATE->GetCurrentGameDef()->m_szName;
		if( THEME->DoesThemeExist( sGameName ) )
			THEME->SwitchTheme( sGameName );
		else
			THEME->SwitchTheme( "default" );
		TEXTUREMAN->DoDelayedDelete();
	}
	PREFSMAN->SaveGamePrefsToDisk();

	if( PREFSMAN->m_bFirstRun )
		SCREENMAN->SetNewScreen( "ScreenAutoGraphicDetail" );
	else
		SCREENMAN->SetNewScreen( THEME->GetMetric("Common","InitialScreen") );
	PREFSMAN->m_bFirstRun = false;

	if( PREFSMAN->m_bAutoMapJoysticks )
		INPUTMAPPER->AutoMapJoysticksForCurrentGame();
}

static void GameLoop();

static bool ChangeAppPri()
{
	if(PREFSMAN->m_iBoostAppPriority == 0)
		return false;

	/* If -1 and this is a debug build, don't.  It makes the debugger sluggish. */
#ifdef DEBUG
	if(PREFSMAN->m_iBoostAppPriority == -1)
		return false;
#endif

	return true;
}

static void BoostAppPri()
{
	if(!ChangeAppPri())
		return;

#ifdef WIN32
	/* We just want a slight boost, so we don't skip needlessly if something happens
	 * in the background.  We don't really want to be high-priority--above normal should
	 * be enough.  However, ABOVE_NORMAL_PRIORITY_CLASS is only supported in Win2000
	 * and later. */
	OSVERSIONINFO version;
	version.dwOSVersionInfoSize=sizeof(version);
	if(!GetVersionEx(&version))
	{
		LOG->Warn(werr_ssprintf(GetLastError(), "GetVersionEx failed"));
		return;
	}

#ifndef ABOVE_NORMAL_PRIORITY_CLASS
#define ABOVE_NORMAL_PRIORITY_CLASS 0x00008000
#endif

	DWORD pri = HIGH_PRIORITY_CLASS;
	if(version.dwMajorVersion >= 5)
		pri = ABOVE_NORMAL_PRIORITY_CLASS;

	/* Be sure to boost the app, not the thread, to make sure the
	 * sound thread stays higher priority than the main thread. */
	SetPriorityClass(GetCurrentProcess(), pri);
#endif
}

#if defined(WIN32)
#include "RageDisplay_D3D.h"
#endif

#if !defined(_XBOX)
#include "RageDisplay_OGL.h"
#endif

/* XXX: Passing all of the SetVideoMode arguments to the ctor is cumbersome. */
#if !defined(_XBOX)
static RageDisplay *CreateDisplay_OGL()
{
	return new RageDisplay_OGL(
		PREFSMAN->m_bWindowed, 
		PREFSMAN->m_iDisplayWidth, 
		PREFSMAN->m_iDisplayHeight, 
		PREFSMAN->m_iDisplayColorDepth, 
		PREFSMAN->m_iRefreshRate,
		PREFSMAN->m_bVsync,
		THEME->GetMetric("Common","WindowTitle"),
		THEME->GetPathToG("Common window icon") );
}
#endif

#if defined(WIN32)
static RageDisplay *CreateDisplay_D3D()
{
	return new RageDisplay_D3D(
		PREFSMAN->m_bWindowed, 
		PREFSMAN->m_iDisplayWidth, 
		PREFSMAN->m_iDisplayHeight, 
		PREFSMAN->m_iDisplayColorDepth, 
		PREFSMAN->m_iRefreshRate,
		PREFSMAN->m_bVsync,
		THEME->GetMetric("Common","WindowTitle"),
		THEME->GetPathToG("Common window icon") );
}
#endif

#if defined(_XBOX)
RageDisplay *CreateDisplay() { return CreateDisplay_D3D(); }
#elif !defined(WIN32)
RageDisplay *CreateDisplay() { return CreateDisplay_OGL(); }
#else

#include "archutils/Win32/VideoDriverInfo.h"
#include "Regex.h"

static CString GetCardName()
{
	/* We can't too reliably get driver information before we set up the window. Ask
	 * VideoDriverInfo; it'll usually give us a name we can match.  XXX: If we're on
	 * an old system with more than one card, we won't know which to match. */
	VideoDriverInfo info;
	GetPrimaryVideoDriverInfo(info);
	return info.sDescription;
}

static bool CardRequiresD3D()
{
	vector<Regex> Cards;
	Cards.push_back(Regex(".*Voodoo.*"));
//	Cards.push_back(Regex(".*nVidia.*")); // testing

	const CString desc = GetCardName();
	for(unsigned i = 0; i < Cards.size(); ++i)
		if(Cards[i].Compare(desc)) return true;

	return false;
}

static const CString D3DURL = "http://search.microsoft.com/gomsuri.asp?n=1&c=rp_BestBets&siteid=us&target=http://www.microsoft.com/downloads/details.aspx?FamilyID=a19bed22-0b25-4e5d-a584-6389d8a3dad0&displaylang=en";

RageDisplay *CreateDisplay()
{
	/* We never want to bother users with having to decide which API to use.
	 *
	 * Some cards simply are too troublesome with OpenGL to ever use it, eg. Voodoos.
	 * If D3D8 isn't installed on those, complain and refuse to run (by default).
	 * For others, always use OpenGL.  Allow forcing to D3D as an advanced option.
	 *
	 * If we're missing acceleration when we load D3D8 due to a card being in the
	 * D3D list, it means we need drivers and that they do exist.
	 *
	 * If we try to load OpenGL and we're missing acceleration, it may mean:
	 *  1. We're missing drivers, and they just need upgrading.
	 *  2. The card doesn't have drivers, and it should be using D3D8.  In other words,
	 *     it needs an entry in this table.
	 *  3. The card doesn't have drivers for either.  (Sorry, no S3 868s.)  Can't play.
	 * 
	 * In this case, fail to load; don't silently fall back on D3D.  We don't want
	 * people unknowingly using D3D8 with old drivers (and reporting obscure bugs
	 * due to driver problems).  We'll probably get bug reports for all three types.
	 * #2 is the only case that's actually a bug.
	 *
	 * Actually, right now we're falling back.  I'm not sure which behavior is better.
	 *
	 * This should probably be exported into an INI.
	 */
	bool NeedsD3D = false;
	if( PREFSMAN->m_sRenderer != "" )
	{
		LOG->Warn("Forcing renderer: %s", PREFSMAN->m_sRenderer.c_str() );
		if( !PREFSMAN->m_sRenderer.CompareNoCase("opengl") )
			NeedsD3D = false;
		else if( !PREFSMAN->m_sRenderer.CompareNoCase("d3d") )
			NeedsD3D = true;
		else 
			RageException::Throw("Unknown Renderer value: %s", PREFSMAN->m_sRenderer.c_str() );
	}

	if( CardRequiresD3D() )
		NeedsD3D = true;

	CString error = "There was an error while initializing your video card.\n\n";
	if( PREFSMAN->m_sRenderer != "" )
		error = "(WARNING: Renderer was forced)\n\n";
	
	error += "   PLEASE DO NOT FILE THIS ERROR AS A BUG!\n\n";
	
	if( NeedsD3D )
	{
		/* We require D3D.  Try to start it.  */
		error += "Your display adapter, \"" + GetCardName() + "\", requires Direct3D 8 (or "
			"higher), but ";

		try {
			return CreateDisplay_D3D();
		} catch(RageException_D3DNotInstalled e) {
			error += 
				"it is not installed.  You can download it from:\n" +
				D3DURL;
		} catch(RageException_D3DNoAcceleration e) {
			error += 
				"your system is reporting that hardware acceleration is not available.  "
				"You can download an updated driver from your card's manufacturer.";
		};


		/* This card is listed as having problems in OpenGL, so don't try it; it'll
		 * just generate bug reports. */
		RageException::Throw( error );
	}

	/* Try to create an OpenGL renderer.  This should always succeed.  (Actually,
	 * SDL may throw, but that only happens with broken driver installations, and
	 * we probably don't want to fall back on D3D in that case anyway.) */
	RageDisplay *ret = CreateDisplay_OGL();
	
	if( PREFSMAN->m_bAllowUnacceleratedRenderer || !ret->IsSoftwareRenderer() )
		return ret;

	/* OpenGL is unaccelerated.  Try D3D. */
	delete ret;

	try {
		return CreateDisplay_D3D();
	} catch(RageException_D3DNotInstalled e) {
		/* Eek.  We don't know if we need newer drivers, D3D8 or both. */
		error += 
			"OpenGL hardware acceleration is not available on your system, and "
			"Direct3D 8 (or higher) is not installed.  You can download it from:\n" +
			D3DURL + "\n\n"
			"You may also need updated drivers from your card's manufacturer.";
	} catch(RageException_D3DNoAcceleration e) {
		error += 
			"Neither OpenGL nor Direct3D hardware acceleration is available on your "
			"system.  Please install the latest video drivers from your graphics "
			"card vendor.";
	};
	RageException::Throw( error );
}

#endif

static void RestoreAppPri()
{
	if(!ChangeAppPri())
		return;

#ifdef WIN32
	SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
#endif
}

int main(int argc, char* argv[])
{
	/* Set up arch hooks first.  This may set up crash handling. */
	HOOKS = MakeArchHooks();

	ChangeToDirOfExecutable(argv[0]);

	/* Set this up second.  Do this early, since it's needed for RageException::Throw. 
	 * Do it after ChangeToDirOfExecutable, so the log ends up in the right place. */
	LOG			= new RageLog();

#ifdef DEBUG
	LOG->ShowConsole();
#endif

	/* Whew--we should be able to crash safely now! */

	atexit(SDL_Quit);   /* Clean up on exit */

	/* Fire up the SDL, but don't actually start any subsystems.
	 * We use our own error handlers. */
	int SDL_flags = SDL_INIT_NOPARACHUTE;
	SDL_Init(SDL_flags);

	CString  g_sErrorString = "";

#ifndef DEBUG
	try{
#endif


	LoadingWindow *loading_window = MakeLoadingWindow();

	loading_window->Paint();

	// changed to use time.  GetTimeSinceStart is silly because it always return 0! -Chris
	srand( time(NULL) );	// seed number generator	
	
	HOOKS->DumpDebugInfo();

	//
	// Create game objects
	//
	GAMESTATE	= new GameState;
	PREFSMAN	= new PrefsManager;
	GAMEMAN		= new GameManager;
	THEME		= new ThemeManager;
	NOTESKIN	= new NoteSkinManager;
	SOUNDMAN	= new RageSoundManager(PREFSMAN->m_sSoundDrivers);
	SOUNDMAN->SetPrefs(PREFSMAN->m_fSoundVolume);
	ANNOUNCER	= new AnnouncerManager;
	INPUTFILTER	= new InputFilter;
	INPUTMAPPER	= new InputMapper;
	INPUTQUEUE	= new InputQueue;
	SONGINDEX	= new SongCacheIndex;
	/* depends on SONGINDEX: */
	SONGMAN		= new SongManager( loading_window );		// this takes a long time to load
	delete loading_window;		// destroy this before init'ing Display

	PREFSMAN->ReadGlobalPrefsFromDisk( true );
	PREFSMAN->ReadGamePrefsFromDisk();

	DISPLAY = CreateDisplay();
	TEXTUREMAN	= new RageTextureManager();
	TEXTUREMAN->SetPrefs( 
		PREFSMAN->m_iTextureColorDepth, 
		PREFSMAN->m_bDelayedTextureDelete, 
		PREFSMAN->m_iMaxTextureResolution );

	/* Now that we've started DISPLAY, we can set up event masks. */
	mySDL_EventState(SDL_QUIT, SDL_ENABLE);
	mySDL_EventState(SDL_ACTIVEEVENT, SDL_ENABLE);

	/* Grab the window manager specific information */
	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	if ( SDL_GetWMInfo(&info) < 0 ) 
		RageException::Throw( "SDL_GetWMInfo failed" );

#ifdef WIN32
	g_hWndMain = info.window;
#endif

	/* This initializes objects that change the SDL event mask, and has other
	 * dependencies on the SDL video subsystem, so it must be initialized after
	 * DISPLAY and setting the default SDL event mask. */
	INPUTMAN	= new RageInput;

	// These things depend on the TextureManager, so do them after!
	FONT		= new FontManager;
	SCREENMAN	= new ScreenManager;

	/* People may want to do something else while songs are loading, so do
	 * this after loading songs. */
	BoostAppPri();

	ResetGame();

	/* Load the unlocks into memory */
	GAMESTATE->UnlockingSys.LoadFromDATFile("Data/Unlocks.dat");

	/* Run the main loop. */
	GameLoop();

	PREFSMAN->SaveGlobalPrefsToDisk();
	PREFSMAN->SaveGamePrefsToDisk();

#ifndef DEBUG
	}
	catch( RageException e )
	{
		g_sErrorString = e.what();
	}
#endif

	SAFE_DELETE( SCREENMAN );
	SAFE_DELETE( INPUTQUEUE );
	SAFE_DELETE( INPUTMAPPER );
	SAFE_DELETE( INPUTFILTER );
	SAFE_DELETE( SONGMAN );
	SAFE_DELETE( SONGINDEX );
	SAFE_DELETE( PREFSMAN );
	SAFE_DELETE( GAMESTATE );
	SAFE_DELETE( GAMEMAN );
	SAFE_DELETE( NOTESKIN );
	SAFE_DELETE( THEME );
	SAFE_DELETE( ANNOUNCER );
	SAFE_DELETE( INPUTMAN );
	SAFE_DELETE( SOUNDMAN );
	SAFE_DELETE( FONT );
	SAFE_DELETE( TEXTUREMAN );
	SAFE_DELETE( DISPLAY );
	SAFE_DELETE( LOG );
	SAFE_DELETE( HOOKS );
	
	if( g_sErrorString != "" )
	{
		// throw up a pretty error dialog
		ErrorDialog *d = MakeErrorDialog();
		d->SetErrorText(g_sErrorString);
		d->ShowError();
		delete d;
	}

	return 0;
}

/* Returns true if the key has been handled and should be discarded, false if
 * the key should be sent on to screens. */
bool HandleGlobalInputs( DeviceInput DeviceI, InputEventType type, GameInput GameI, MenuInput MenuI, StyleInput StyleI )
{
	/* None of the globals keys act on types other than FIRST_PRESS */
	if( type != IET_FIRST_PRESS ) 
		return false;

	switch( MenuI.button )
	{
	case MENU_BUTTON_OPERATOR:

		/* Global operator key, to get quick access to the options menu. Don't
		 * do this if we're on a "system menu", which includes the editor
		 * (to prevent quitting without storing changes). */
		if( !GAMESTATE->m_bIsOnSystemMenu )
		{
			SCREENMAN->SystemMessage( "OPERATOR" );
			SCREENMAN->SetNewScreen( "ScreenOptionsMenu" );
		}
		return true;

	case MENU_BUTTON_COIN:
		/* Handle a coin insertion. */
		if( GAMESTATE->m_bEditing )	// no coins while editing
			break;
		GAMESTATE->m_iCoins++;
		SCREENMAN->RefreshCreditsMessages();
		SOUNDMAN->PlayOnce( THEME->GetPathToS("Common coin") );
		return false;	// Attract need to know because they go to TitleMenu on > 1 credit
	}

	if(DeviceI == DeviceInput(DEVICE_KEYBOARD, SDLK_F4))
	{
		if( INPUTFILTER->IsBeingPressed( DeviceInput(DEVICE_KEYBOARD, SDLK_RALT)) ||
			INPUTFILTER->IsBeingPressed( DeviceInput(DEVICE_KEYBOARD, SDLK_LALT)) )
		{
			// pressed Alt+F4
			ExitGame();
			return true;
		}
	}

	if(DeviceI == DeviceInput(DEVICE_KEYBOARD, SDLK_F5))
	{
		// Save Screenshot.
		CString sPath;
		for( int i=0; i<1000; i++ )
		{
			sPath = ssprintf("screen%04d.bmp",i);
			if( !DoesFileExist(sPath) )
				break;
		}
		DISPLAY->SaveScreenshot( sPath );
		SCREENMAN->SystemMessage( "Saved screenshot: " + sPath );
		return true;
	}

	if(DeviceI == DeviceInput(DEVICE_KEYBOARD, SDLK_RETURN))
	{
		if( INPUTFILTER->IsBeingPressed(DeviceInput(DEVICE_KEYBOARD, SDLK_RALT)) ||
			INPUTFILTER->IsBeingPressed(DeviceInput(DEVICE_KEYBOARD, SDLK_LALT)) )
		{
			/* alt-enter */
			PREFSMAN->m_bWindowed = !PREFSMAN->m_bWindowed;
			ApplyGraphicOptions();
			return true;
		}
	}

	return false;
}

static void HandleInputEvents(float fDeltaTime)
{
	INPUTFILTER->Update( fDeltaTime );
	
	static InputEventArray ieArray;
	ieArray.clear();	// empty the array
	INPUTFILTER->GetInputEvents( ieArray );
	for( unsigned i=0; i<ieArray.size(); i++ )
	{
		DeviceInput DeviceI = (DeviceInput)ieArray[i];
		InputEventType type = ieArray[i].type;
		GameInput GameI;
		MenuInput MenuI;
		StyleInput StyleI;

		INPUTMAPPER->DeviceToGame( DeviceI, GameI );
		
		if( GameI.IsValid()  &&  type == IET_FIRST_PRESS )
			INPUTQUEUE->RememberInput( GameI );
		if( GameI.IsValid() )
		{
			INPUTMAPPER->GameToMenu( GameI, MenuI );
			INPUTMAPPER->GameToStyle( GameI, StyleI );
		}

		// HACK:  Numlock is read is being pressed if the NumLock light is on.
		// Filter out all NumLock repeat messages
		if( DeviceI.device == DEVICE_KEYBOARD && DeviceI.button == SDLK_NUMLOCK && type != IET_FIRST_PRESS )
			continue;	// skip

		if( HandleGlobalInputs(DeviceI, type, GameI, MenuI, StyleI ) )
			continue;	// skip
		
		SCREENMAN->Input( DeviceI, type, GameI, MenuI, StyleI );
	}
}

static void HandleSDLEvents()
{
	// process all queued events
	SDL_Event event;
	while(SDL_GetEvent(event, SDL_QUITMASK|SDL_ACTIVEEVENTMASK))
	{
		switch(event.type)
		{
		case SDL_QUIT:
			LOG->Trace("SDL_QUIT: shutting down");
			ExitGame();
			break;

		case SDL_ACTIVEEVENT:
			{
				/* We don't care about mouse focus. */
				if(event.active.state == SDL_APPMOUSEFOCUS)
					break;

				Uint8 i = SDL_GetAppState();
				
				g_bHasFocus = i&SDL_APPINPUTFOCUS && i&SDL_APPACTIVE;
				LOG->Trace("App %s focus (%i%i)", g_bHasFocus? "has":"doesn't have",
					i&SDL_APPINPUTFOCUS, i&SDL_APPACTIVE);

				if(g_bHasFocus)
					BoostAppPri();
				else
					RestoreAppPri();
			}
		}
	}
}

static void GameLoop()
{
	RageTimer timer;
	while(!g_bQuitting)
	{
		/* This needs to be called before anything that handles SDL events. */
		SDL_PumpEvents();
		HandleSDLEvents();

		/*
		 * Update
		 */
		float fDeltaTime = timer.GetDeltaTime();
		
		if( INPUTFILTER->IsBeingPressed( DeviceInput(DEVICE_KEYBOARD, SDLK_TAB) ) ) {
			if( INPUTFILTER->IsBeingPressed( DeviceInput(DEVICE_KEYBOARD, SDLK_BACKQUOTE) ) )
				fDeltaTime = 0; /* both; stop time */
			else
				fDeltaTime *= 4;
		} else
			if( INPUTFILTER->IsBeingPressed( DeviceInput(DEVICE_KEYBOARD, SDLK_BACKQUOTE) ) )
				fDeltaTime /= 4;

		DISPLAY->Update( fDeltaTime );
		TEXTUREMAN->Update( fDeltaTime );
		GAMESTATE->Update( fDeltaTime );
		SCREENMAN->Update( fDeltaTime );
		SOUNDMAN->Update( fDeltaTime );

		/* Important:  Process input AFTER updating game logic, or input will be acting on song beat from last frame */
		HandleInputEvents( fDeltaTime );

		/*
		 * Render
		 */
		SCREENMAN->Draw();

		if(g_bHasFocus)
			SDL_Delay( 1 );	// give some time to other processes and threads
		else
			SDL_Delay( 10 );// give some time to other processes and threads
	}
}
