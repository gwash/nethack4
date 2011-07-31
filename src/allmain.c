/* Copyright (c) Stichting Mathematisch Centrum, Amsterdam, 1985. */
/* NetHack may be freely redistributed.  See license for details. */

/* various code that was replicated in *main.c */

#include "hack.h"

#include <sys/stat.h>

#include "dlb.h"
#include "hack.h"
#include "patchlevel.h"

static const char *copyright_banner[] =
{COPYRIGHT_BANNER_A, COPYRIGHT_BANNER_B, COPYRIGHT_BANNER_C};

static void wd_message(void);
static void pre_move_tasks(boolean didmove);

static void wd_message(void)
{
    if (discover)
	You("are in non-scoring discovery mode.");
}


const char **nh_get_copyright_banner(void)
{
    return copyright_banner;
}

void nh_init(int pid, struct window_procs *procs, char **paths)
{
    int i;
            
    if (!api_entry_checkpoint()) /* not sure anything in here can actually call panic */
	return;
    
    hackpid = pid;
    windowprocs = *procs;
    
    for (i = 0; i < PREFIX_COUNT; i++)
	fqn_prefix[i] = paths[i];
    
    program_state.game_running = 0;
    initoptions();

    dlb_init();	/* must be before newgame() */

    /*
	* Initialization of the boundaries of the mazes
	* Both boundaries have to be even.
	*/
    x_maze_max = COLNO-1;
    if (x_maze_max % 2)
	    x_maze_max--;
    y_maze_max = ROWNO-1;
    if (y_maze_max % 2)
	    y_maze_max--;

    /*
	*  Initialize the vision system.  This must be before mklev() on a
	*  new game or before a level restore on a saved game.
	*/
    vision_init();
    
    u.uhp = 1;	/* prevent RIP on early quits */
    
    api_exit();
}


boolean nh_exit(int exit_type)
{
    if (!api_entry_checkpoint()) /* not sure anything in here can actually call panic */
	return TRUE; /* terminate was called, so exit is successful */
	
    xmalloc_cleanup();
    
    if (program_state.game_running) {
	switch (exit_type) {
	    case EXIT_REQUEST_SAVE:
		dosave(); /* will ask "really save?" and, if 'y', eventually call terminate. */
		break;
		
	    case EXIT_FORCE_SAVE:
		dosave0(TRUE);
		break; /* only reached if saving fails */
		
	    case EXIT_REQUEST_QUIT:
		done2();
		break;
		    
	    case EXIT_FORCE_QUIT:
		done(QUIT);
		break; /* not reached */
		
	    case EXIT_PANIC:
		/* freeing things should be safe */
		freedynamicdata();
		dlb_cleanup();
		panic("UI problem.");
		break;
	}
	
	api_exit();
	return FALSE;
    }
    
    clearlocks();
    /* calling terminate() will get us out of nested contexts safely, eg:
     * UI_cmdloop -> nh_do_move -> UI_update_screen (problem happens here) -> nh_exit
     * will jump all the way back to UI_cmdloop */
    terminate(EXIT_FAILURE);
    
    api_exit(); /* not reached */
    return TRUE;
}


static void startup_common(char *name, int locknum, int playmode)
{
    if (playmode == MODE_EXPLORE)
	discover = TRUE;
    else if (playmode == MODE_WIZARD)
	wizard = TRUE;
    
    if (name && name[0])
	strcpy(plname, name);

    if (wizard)
	    strcpy(plname, "wizard");

    sprintf(lock, "%d%s", (int)getuid(), plname);
    getlock(locknum);

    display_gamewindows();

    initrack();
}


static void post_init_tasks(void)
{
    flags.moonphase = phase_of_the_moon();
    if (flags.moonphase == FULL_MOON) {
	You("are lucky!  Full moon tonight.");
	change_luck(1);
    } else if (flags.moonphase == NEW_MOON) {
	pline("Be careful!  New moon tonight.");
    }
    flags.friday13 = friday_13th();
    if (flags.friday13) {
	pline("Watch out!  Bad things can happen on Friday the 13th.");
	change_luck(-1);
    }

    encumber_msg(); /* in case they auto-picked up something */

    u.uz0.dlevel = u.uz.dlevel;
    youmonst.movement = NORMAL_SPEED;	/* give the hero some movement points */
    
    /* prepare for the first move */
    pre_move_tasks(FALSE);
}


boolean nh_restore_save(char *name, int locknum, int playmode)
{
    int fd;
    
    if (!api_entry_checkpoint())
	return FALSE;
    
     startup_common(name, locknum, playmode);
    
    fd = restore_saved_game();
    if (fd < 0)
	goto not_recovered;
	
    /* Since wizard is actually flags.debug, restoring might
	* overwrite it.
	*/
    boolean remember_wiz_mode = wizard;
    const char *fq_save = fqname(SAVEF, SAVEPREFIX, 1);

    chmod(fq_save,0);	/* disallow parallel restores */

    pline("Restoring save file...");
    if (!dorecover(fd))
	    goto not_recovered;
    if (!wizard && remember_wiz_mode) wizard = TRUE;
    check_special_room(FALSE);
    wd_message();

    if (discover || wizard) {
	    if (yn("Do you want to keep the save file?") == 'n')
		delete_savefile();
	    else {
		chmod(fq_save,FCMASK); /* back to readable */
	    }
    }
    flags.move = 0;
    
    sync_options();
    
    program_state.game_running = 1;
    post_init_tasks();
    api_exit();
    return TRUE;
not_recovered:
    
    clearlocks();
    program_state.game_running = 0;
    api_exit();
    return FALSE;
}


void nh_start_game(char *name, int locknum, int playmode)
{
    if (!api_entry_checkpoint())
	return;
    
    moves = monstermoves = 1;
    
    startup_common(name, locknum, playmode);
    
    /* prevent an unnecessary prompt in player selection */
    rigid_role_checks();
    player_selection(flags.initrole, flags.initrace, flags.initgend,
	flags.initalign, flags.randomall);
    newgame();
    wd_message();

    flags.move = 0;
    set_wear();
    pickup(1);
    
    program_state.game_running = 1;
    post_init_tasks();
    
    api_exit();
}


static void you_moved(void)
{
    int moveamt = 0, wtcap = 0, change = 0;
    boolean monscanmove = FALSE;
    
    /* actual time passed */
    youmonst.movement -= NORMAL_SPEED;

    do { /* hero can't move this turn loop */
	wtcap = encumber_msg();

	flags.mon_moving = TRUE;
	do {
	    monscanmove = movemon();
	    if (youmonst.movement > NORMAL_SPEED)
		break;	/* it's now your turn */
	} while (monscanmove);
	flags.mon_moving = FALSE;

	if (!monscanmove && youmonst.movement < NORMAL_SPEED) {
	    /* both you and the monsters are out of steam this round */
	    /* set up for a new turn */
	    struct monst *mtmp;
	    mcalcdistress();	/* adjust monsters' trap, blind, etc */

	    /* reallocate movement rations to monsters */
	    for (mtmp = level.monlist; mtmp; mtmp = mtmp->nmon)
		mtmp->movement += mcalcmove(mtmp);

	    if (!rn2(u.uevent.udemigod ? 25 :
		    (depth(&u.uz) > depth(&stronghold_level)) ? 50 : 70))
		makemon(NULL, 0, 0, NO_MM_FLAGS);

	    /* calculate how much time passed. */
	    if (u.usteed && u.umoved) {
		/* your speed doesn't augment steed's speed */
		moveamt = mcalcmove(u.usteed);
	    } else {
		moveamt = youmonst.data->mmove;

		if (Very_fast) {	/* speed boots or potion */
		    /* average movement is 1.67 times normal */
		    moveamt += NORMAL_SPEED / 2;
		    if (rn2(3) == 0) moveamt += NORMAL_SPEED / 2;
		} else if (Fast) {
		    /* average movement is 1.33 times normal */
		    if (rn2(3) != 0) moveamt += NORMAL_SPEED / 2;
		}
	    }

	    switch (wtcap) {
		case UNENCUMBERED: break;
		case SLT_ENCUMBER: moveamt -= (moveamt / 4); break;
		case MOD_ENCUMBER: moveamt -= (moveamt / 2); break;
		case HVY_ENCUMBER: moveamt -= ((moveamt * 3) / 4); break;
		case EXT_ENCUMBER: moveamt -= ((moveamt * 7) / 8); break;
		default: break;
	    }

	    youmonst.movement += moveamt;
	    if (youmonst.movement < 0) youmonst.movement = 0;
	    settrack();

	    monstermoves++;
	    moves++;

	    /********************************/
	    /* once-per-turn things go here */
	    /********************************/

	    if (flags.bypasses) clear_bypasses();
	    if (Glib) glibr();
	    nh_timeout();
	    run_regions();

	    if (u.ublesscnt)  u.ublesscnt--;
	    botl = 1;

	    /* One possible result of prayer is healing.  Whether or
		* not you get healed depends on your current hit points.
		* If you are allowed to regenerate during the prayer, the
		* end-of-prayer calculation messes up on this.
		* Another possible result is rehumanization, which requires
		* that encumbrance and movement rate be recalculated.
		*/
	    if (u.uinvulnerable) {
		/* for the moment at least, you're in tiptop shape */
		wtcap = UNENCUMBERED;
	    } else if (Upolyd && youmonst.data->mlet == S_EEL && !is_pool(u.ux,u.uy) && !Is_waterlevel(&u.uz)) {
		if (u.mh > 1) {
		    u.mh--;
		    botl = 1;
		} else if (u.mh < 1)
		    rehumanize();
	    } else if (Upolyd && u.mh < u.mhmax) {
		if (u.mh < 1)
		    rehumanize();
		else if (Regeneration ||
			    (wtcap < MOD_ENCUMBER && !(moves%20))) {
		    botl = 1;
		    u.mh++;
		}
	    } else if (u.uhp < u.uhpmax &&
		    (wtcap < MOD_ENCUMBER || !u.umoved || Regeneration)) {
		if (u.ulevel > 9 && !(moves % 3)) {
		    int heal, Con = (int) ACURR(A_CON);

		    if (Con <= 12) {
			heal = 1;
		    } else {
			heal = rnd(Con);
			if (heal > u.ulevel-9) heal = u.ulevel-9;
		    }
		    botl = 1;
		    u.uhp += heal;
		    if (u.uhp > u.uhpmax)
			u.uhp = u.uhpmax;
		} else if (Regeneration ||
			(u.ulevel <= 9 &&
			!(moves % ((MAXULEV+12) / (u.ulevel+2) + 1)))) {
		    botl = 1;
		    u.uhp++;
		}
	    }

	    /* moving around while encumbered is hard work */
	    if (wtcap > MOD_ENCUMBER && u.umoved) {
		if (!(wtcap < EXT_ENCUMBER ? moves%30 : moves%10)) {
		    if (Upolyd && u.mh > 1) {
			u.mh--;
		    } else if (!Upolyd && u.uhp > 1) {
			u.uhp--;
		    } else {
			You("pass out from exertion!");
			exercise(A_CON, FALSE);
			fall_asleep(-10, FALSE);
		    }
		}
	    }

	    if ((u.uen < u.uenmax) &&
		((wtcap < MOD_ENCUMBER &&
		    (!(moves%((MAXULEV + 8 - u.ulevel) *
			    (Role_if (PM_WIZARD) ? 3 : 4) / 6))))
		    || Energy_regeneration)) {
		u.uen += rn1((int)(ACURR(A_WIS) + ACURR(A_INT)) / 15 + 1,1);
		if (u.uen > u.uenmax)  u.uen = u.uenmax;
		botl = 1;
	    }

	    if (!u.uinvulnerable) {
		if (Teleportation && !rn2(85)) {
		    xchar old_ux = u.ux, old_uy = u.uy;
		    tele();
		    if (u.ux != old_ux || u.uy != old_uy) {
			if (!next_to_u()) {
			    check_leash(old_ux, old_uy);
			}
		    }
		}
		/* delayed change may not be valid anymore */
		if ((change == 1 && !Polymorph) ||
		    (change == 2 && u.ulycn == NON_PM))
		    change = 0;
		if (Polymorph && !rn2(100))
		    change = 1;
		else if (u.ulycn >= LOW_PM && !Upolyd &&
			    !rn2(80 - (20 * night())))
		    change = 2;
		if (change && !Unchanging) {
		    if (multi >= 0) {
			if (occupation)
			    stop_occupation();
			else
			    nomul(0);
			if (change == 1) polyself(FALSE);
			else you_were();
			change = 0;
		    }
		}
	    }

	    if (Searching && multi >= 0) dosearch0(1);
	    dosounds();
	    do_storms();
	    gethungry();
	    age_spells();
	    exerchk();
	    invault();
	    if (u.uhave.amulet) amulet();
	    if (!rn2(40+(int)(ACURR(A_DEX)*3)))
		u_wipe_engr(rnd(3));
	    if (u.uevent.udemigod && !u.uinvulnerable) {
		if (u.udg_cnt) u.udg_cnt--;
		if (!u.udg_cnt) {
		    intervene();
		    u.udg_cnt = rn1(200, 50);
		}
	    }
	    restore_attrib();
	    /* underwater and waterlevel vision are done here */
	    if (Is_waterlevel(&u.uz))
		movebubbles();
	    else if (Underwater)
		under_water(0);
	    /* vision while buried done here */
	    else if (u.uburied) under_ground(0);

	    /* when immobile, count is in turns */
	    if (multi < 0) {
		if (++multi == 0) {	/* finished yet? */
		    unmul(NULL);
		    /* if unmul caused a level change, take it now */
		    if (u.utotype) deferred_goto();
		}
	    }
	}
    } while (youmonst.movement<NORMAL_SPEED); /* hero can't move loop */

    /******************************************/
    /* once-per-hero-took-time things go here */
    /******************************************/

}


static void handle_occupation(void)
{
#if defined(WIN32)
    char ch;
    int abort_lev = 0;
    if (kbhit()) {
	if ((ch = nhgetch()) == ABORT)
	    abort_lev++;
    }
    if (!abort_lev && (*occupation)() == 0)
#else
    if ((*occupation)() == 0)
#endif
	occupation = 0;
    if (
#if defined(WIN32)
	    abort_lev ||
#endif
	    monster_nearby()) {
	stop_occupation();
	reset_eat();
    }
#if defined(WIN32)
    if (!(++occtime % 7))
	display_nhwindow(NHW_MAP, FALSE);
#endif    
}


static void handle_lava_trap(boolean didmove)
{
    if (!is_lava(u.ux,u.uy))
	u.utrap = 0;
    else if (!u.uinvulnerable) {
	u.utrap -= 1<<8;
	if (u.utrap < 1<<8) {
	    killer_format = KILLED_BY;
	    killer = "molten lava";
	    You("sink below the surface and die.");
	    done(DISSOLVED);
	} else if (didmove && !u.umoved) {
	    Norep("You sink deeper into the lava.");
	    u.utrap += rnd(4);
	}
    }
}


static void special_vision_handling(void)
{
    /* redo monsters if hallu or wearing a helm of telepathy */
    if (Hallucination) {	/* update screen randomly */
	see_monsters();
	see_objects();
	see_traps();
	if (u.uswallow) swallowed(0);
    } else if (Unblind_telepat) {
	see_monsters();
    } else if (Warning || Warn_of_mon)
	see_monsters();

    if (vision_full_recalc)
	vision_recalc(0);	/* vision! */
}


static void pre_move_tasks(boolean didmove)
{
    find_ac();
    if (!flags.mv || Blind)
	special_vision_handling();
    
    if (botl || botlx)
	bot();

    if ((u.uhave.amulet || Clairvoyant) &&
	!In_endgame(&u.uz) && !BClairvoyant &&
	!(moves % 15) && !rn2(2))
	    do_vicinity_map();

    if (u.utrap && u.utraptype == TT_LAVA)
	handle_lava_trap(didmove);

    if (iflags.sanity_check)
	sanity_check();

    u.umoved = FALSE;

    if (multi > 0) {
	lookaround(0, 0);
	if (!multi) {
	    /* lookaround may clear multi */
	    flags.move = 0;
	    botl = 1;
	}
    }
}


int nh_do_move(const char *cmd, int rep, struct nh_cmd_arg *arg)
{
    boolean didmove = FALSE;
    
    if (!program_state.game_running)
	return ERR_GAME_NOT_RUNNING;
    
    if (!api_entry_checkpoint()) {
	/* terminate() in end.c will arrive here */
	if (program_state.panicking)
	    return GAME_PANICKED;
	if (!program_state.gameover)
	    return GAME_SAVED;
	return GAME_OVER;
    }
    
    
    if (multi >= 0 && occupation)
	handle_occupation();
    else if (multi == 0) {
	do_command(cmd, rep, TRUE, arg);
    } else if (multi > 0) {
	if (cmd)
	    return ERR_NO_INPUT_ALLOWED;
	
	/* allow interruption of multi-turn commands */
	if (rep == -1) {
	    nomul(0);
	    return READY_FOR_INPUT;
	}
	
	if (flags.mv) {
	    if (multi < COLNO && !--multi)
		flags.travel = iflags.travel1 = flags.mv = flags.run = 0;
	    domove(0, 0, 0);
	} else
	    do_command(saved_cmd, multi, FALSE, arg);
    }
    /* no need to do anything here for multi < 0 */
    
    if (u.utotype)		/* change dungeon level */
	deferred_goto();	/* after rhack() */
    /* !flags.move here: multiple movement command stopped */
    else if (!flags.move || !flags.mv)
	botl = 1;

    if (vision_full_recalc)
	vision_recalc(0);	/* vision! */
    /* when running in non-tport mode, this gets done through domove() */
    if ((!flags.run || iflags.runmode == RUN_TPORT) &&
	    (multi && (!flags.travel ? !(multi % 7) : !(moves % 7L)))) {
	if (flags.run)
	    botl = 1;
	display_nhwindow(NHW_MAP, FALSE);
    }
    
    didmove = flags.move;
    if (didmove) {
	you_moved();
    } /* actual time passed */

    /****************************************/
    /* once-per-player-input things go here */
    /****************************************/
    xmalloc_cleanup();
    
    /* prepare for the next move */
    flags.move = 1;
    pre_move_tasks(didmove);
    flush_screen(1); /* Flush screen buffer */
    
    api_exit();
    /*
     * performing a command can put the game into several different states:
     *  - the command completes immediately: a simple move or an attack etc
     *    multi == 0, occupation == NULL
     *  - if a count is given, the command will (usually) take count turns
     *    multi == count (> 0), occupation == NULL
     *  - the command may cause a delay: for ex. putting on or removing armor
     *    multi == -delay (< 0), occupation == NULL
     *    multi is incremented in you_moved
     *  - the command may take multiple moves, and require a callback to be
     *    run for each move. example: forcing a lock
     *    multi >= 0, occupation == callback
     */
    if (multi >= 0 && occupation)
	return OCCUPATION_IN_PROGRESS;
    else if (multi > 0)
	return MULTI_IN_PROGRESS;
    else if (multi < 0)
	return POST_ACTION_DELAY;
    
    return READY_FOR_INPUT;
}


void stop_occupation(void)
{
    if (occupation) {
	if (!maybe_finished_meal(TRUE))
	    You("stop %s.", occtxt);
	occupation = 0;
	botl = 1; /* in case u.uhs changed */
	nomul(0);
	/* fainting stops your occupation, there's no reason to sync.
	sync_hunger();
	 */
    }
}


void display_gamewindows(void)
{
    /*
     * The mac port is not DEPENDENT on the order of these
     * displays, but it looks a lot better this way...
     */
    display_nhwindow(NHW_STATUS, FALSE);
    display_nhwindow(NHW_MESSAGE, FALSE);
    clear_display_buffer();
    display_nhwindow(NHW_MAP, FALSE);
}

void newgame(void)
{
	int i;

	flags.ident = 1;

	for (i = 0; i < NUMMONS; i++)
		mvitals[i].mvflags = mons[i].geno & G_NOCORPSE;

	init_objects();		/* must be before u_init() */

	flags.pantheon = -1;	/* role_init() will reset this */
	role_init();		/* must be before init_dungeons(), u_init(),
				 * and init_artifacts() */

	init_dungeons();	/* must be before u_init() to avoid rndmonst()
				 * creating odd monsters for any tins and eggs
				 * in hero's initial inventory */
	init_artifacts();
	u_init();

	load_qtlist();	/* load up the quest text info */
/*	quest_init();*/	/* Now part of role_init() */

	mklev();
	u_on_upstairs();
	vision_reset();		/* set up internals for level (after mklev) */
	check_special_room(FALSE);

	botlx = 1;

	/* Move the monster from under you or else
	 * makedog() will fail when it calls makemon().
	 *			- ucsfcgl!kneller
	 */
	if (MON_AT(u.ux, u.uy)) mnexto(m_at(u.ux, u.uy));
	makedog();
	docrt();
	
	/* help the window port get it's display charset/tiles sorted out */
	notify_levelchange();

	if (flags.legacy) {
		flush_screen(1);
		com_pager(1);
	}

#ifdef INSURANCE
	save_currentstate();
#endif
	program_state.something_worth_saving++;	/* useful data now exists */

	/* Success! */
	welcome(TRUE);
	return;
}

/* show "welcome [back] to nethack" message at program startup */
void welcome(
    boolean new_game	/* false => restoring an old game */
    )
{
    char buf[BUFSZ];
    boolean currentgend = Upolyd ? u.mfemale : flags.female;

    /*
     * The "welcome back" message always describes your innate form
     * even when polymorphed or wearing a helm of opposite alignment.
     * Alignment is shown unconditionally for new games; for restores
     * it's only shown if it has changed from its original value.
     * Sex is shown for new games except when it is redundant; for
     * restores it's only shown if different from its original value.
     */
    *buf = '\0';
    if (new_game || u.ualignbase[A_ORIGINAL] != u.ualignbase[A_CURRENT])
	sprintf(eos(buf), " %s", align_str(u.ualignbase[A_ORIGINAL]));
    if (!urole.name.f &&
	    (new_game ? (urole.allow & ROLE_GENDMASK) == (ROLE_MALE|ROLE_FEMALE) :
	     currentgend != flags.initgend))
	sprintf(eos(buf), " %s", genders[currentgend].adj);

    pline(new_game ? "%s %s, welcome to NetHack!  You are a%s %s %s."
		   : "%s %s, the%s %s %s, welcome back to NetHack!",
	  Hello(NULL), plname, buf, urace.adj,
	  (currentgend && urole.name.f) ? urole.name.f : urole.name.m);
}

/*allmain.c*/
