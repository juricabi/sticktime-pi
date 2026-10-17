"""sticktime-update's command line. One run at a time (the lock); progress and the result go to
/run/sticktime/update.json for the web UI, a summary to stdout, details to stderr (the journal)."""
import argparse
import json
import os
import sys
import time
import traceback

from . import VERSION, system
from .app import App
from .common import (EXIT_BUSY, EXIT_INTERNAL, EXIT_OK, EXIT_UPDATE, Config, Lock, Status, UpdateError, fs_error,
                     log, read_settings, truthy)
from .game import Game
from .net import Http

# app failures that only mean "no app updates here" (private repo, no token): not an error at boot
APP_QUIET = ("token_missing", "no_access", "not_found", "token_rejected", "token_forbidden")
NETWORK = ("offline", "timeout", "clock_not_set", "unexpected_response", "rate_limited", "server_error",
           "download_incomplete")


def parser():
    p = argparse.ArgumentParser(prog="sticktime-update", description="Updates of StickTime (the game) and StickTime Pi.")
    p.add_argument("--base", help="the StickTime folder on the SD card (/boot/firmware/sticktime)")
    p.add_argument("--settings", help="the settings file (/boot/firmware/sticktime.txt)")
    p.add_argument("--state", help="the runtime folder (/run/sticktime)")
    p.add_argument("--self-test", action="store_true", help="print 'ok <version>' and exit")
    sub = p.add_subparsers(dest="cmd")

    def what(sp, choices, default=None):
        sp.add_argument("what", nargs="?", choices=choices, default=None)
        # a flag of its own: an absent positional would overwrite a shared dest with its default
        sp.add_argument("--game", dest="what_flag", action="store_const", const="game")
        sp.add_argument("--app", dest="what_flag", action="store_const", const="app")
        sp.set_defaults(what_default=default)

    c = sub.add_parser("check", help="look for updates (stages the newest game for the editor's Install); exit 10 when one is available")
    what(c, ["game", "app", "all"], "all")
    c.add_argument("--json", action="store_true", help="print update.json")
    c.add_argument("--no-cache", action="store_true", help="ask GitHub even if a check ran in the last 10 minutes")
    a = sub.add_parser("apply", help="install the newest game or app")
    what(a, ["game", "app", "all"])
    a.add_argument("--version", help="a given version instead of the newest")
    a.add_argument("--force", action="store_true", help="also go back to an older version, or reinstall")
    a.add_argument("--allow-downgrade", dest="force", action="store_true", help=argparse.SUPPRESS)
    a.add_argument("--auto", action="store_true", help="as the timer: never replace your own game, wait an hour after a release")
    a.add_argument("--no-cache", action="store_true")
    r = sub.add_parser("rollback", help="go back to the previous game or app")
    what(r, ["game", "app"])
    sub.add_parser("status", help="print update.json")
    b = sub.add_parser("boot", help="at boot: finish or undo an interrupted update; then, when auto_update is on, update")
    b.add_argument("--no-network", action="store_true", help="only the repair")
    sub.add_parser("repair", help="the repair part of boot (no network)")
    y = sub.add_parser("sync", help="make the system scripts match the installed app's lib/ (the host runs it at start)")
    y.add_argument("--to", help="the folder they are in (/usr/lib/sticktime)")
    y.add_argument("--wait", type=float, default=120, help="seconds to wait for a running update (120)")
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    if args.self_test:
        print("ok %s" % VERSION)
        return EXIT_OK
    if not args.cmd:
        parser().print_help(sys.stderr)
        return 2
    if hasattr(args, "what"):
        if args.what and args.what_flag and args.what != args.what_flag:
            print("sticktime-update %s: game or app, not both" % args.cmd, file=sys.stderr)
            return 2
        args.what = args.what or args.what_flag or args.what_default
    if args.cmd in ("apply", "rollback") and not args.what:
        print("sticktime-update %s: say what: game or app" % args.cmd, file=sys.stderr)
        return 2
    if args.cmd == "apply" and args.version and args.what == "all":
        print("sticktime-update apply: --version needs game or app", file=sys.stderr)
        return 2
    cfg = Config(args.base, args.settings, args.state)
    if args.cmd == "status":
        return cmd_status(cfg)
    if args.cmd == "sync":
        return cmd_sync(cfg, args)
    lock = Lock(cfg.s("update.lock"))
    if not lock.acquire():
        print("Another update is running.", file=sys.stderr)
        return EXIT_BUSY
    app = st = None
    try:
        st = Status(cfg)
        settings = read_settings(cfg.settings)
        http = Http(cfg)
        game = Game(cfg, http, st)
        app = App(cfg, http, st, settings)
        try:
            if args.cmd in ("check", "apply", "rollback"):
                tidy(game, app)
            return COMMANDS[args.cmd](cfg, st, game, app, settings, args)
        except UpdateError as e:
            st.failed(e)
            print(e.message, file=sys.stderr)
            return e.exit_code
        except OSError as e:
            err = fs_error(e)
            if err is None:
                raise
            st.failed(err)
            print(err.message, file=sys.stderr)
            return err.exit_code
    except Exception as e:
        traceback.print_exc()
        msg = "Internal error in the updater: %s" % e
        try:
            Status(cfg).set(state="error", message=msg, error=msg, progress=0)
        except Exception:
            pass
        return EXIT_INTERNAL
    finally:
        # last: sticktime.service's restart may take this process with it (it runs in the host's cgroup)
        try:
            if app is not None and app.need_restart:
                system.sync_after_app(cfg, st)      # the system scripts of the set now installed
                app.restart(auto=getattr(args, "auto", False) or args.cmd in ("boot", "repair"))
        except Exception as e:
            log("restart: %s" % e)        # the update itself is done; the next start runs it
        finally:
            lock.release()


def tidy(game, app):
    """what a killed run (no power cut, so no boot repair) left: finish its app swap, drop its leftovers"""
    for note in app.repair() + game.cleanup():
        log(note)


def wait_for_time(cfg, limit):
    """systemd-timesyncd: TLS fails while the clock is still at the image's build date"""
    if not sys.platform.startswith("linux") or limit <= 0 or not os.path.isdir("/run/systemd/timesync"):
        return
    t_end = time.monotonic() + limit
    while not os.path.exists("/run/systemd/timesync/synchronized") and time.monotonic() < t_end:
        time.sleep(1)


def refresh_installed(st, game, app):
    st.part("game", installed=game.installed().version)
    stg = game.staged()
    st.part("game", staged=stg["version"] if stg else "")
    st.part("app", installed=app.installed())


def cmd_status(cfg):
    lock = Lock(cfg.s("update.lock"))
    st = Status(cfg, write=False)
    if lock.acquire():
        try:
            st.write = True
            http = Http(cfg)
            refresh_installed(st, Game(cfg, http, st), App(cfg, http, st, {}))
            if st.d["state"] not in ("done", "error", "idle"):
                st.d["state"] = "idle"           # a run that died: nothing is running now
            st.save()
        except OSError as e:
            log("status: %s" % e)
        finally:
            lock.release()
    print(json.dumps(st.d, indent=1))
    return EXIT_OK


def cmd_sync(cfg, args):
    """the host, at every start: waits for a running update (which syncs itself when it changed the app)"""
    lock = Lock(cfg.s("update.lock"))
    end = time.monotonic() + max(0.0, args.wait)
    while not lock.acquire():
        if time.monotonic() >= end:
            print("An update is running: the system scripts are synced at the next start.", file=sys.stderr)
            return EXIT_BUSY
        time.sleep(2)
    try:
        r = system.sync(cfg, args.to)
    except UpdateError as e:
        log(e.message)
        print(e.message, file=sys.stderr)
        return e.exit_code
    except OSError as e:
        log("system scripts: %s" % e)
        print("The system scripts couldn't be updated: %s" % e, file=sys.stderr)
        return EXIT_INTERNAL
    finally:
        lock.release()
    if not r:
        print(r.summary())          # a change is in the log already (stderr: the journal)
    return EXIT_OK


def cmd_check(cfg, st, game, app, settings, args):
    wait_for_time(cfg, min(cfg.time_wait, 15))
    st.set("checking", "Checking for updates…", 0.05, error="")
    refresh_installed(st, game, app)
    avail_any, msgs = False, []
    if args.what in ("game", "all"):
        avail, msg = game.check(use_cache=not args.no_cache)
        avail_any |= avail
        msgs.append(msg)
    if args.what in ("app", "all"):
        try:
            avail, msg = app.check(use_cache=not args.no_cache)
            avail_any |= avail
            msgs.append(msg)
        except UpdateError as e:
            if args.what == "app":
                raise
            msgs.append(e.message)
    st.checked()
    st.set("done", " ".join(msgs), 1, error="")
    print(json.dumps(st.d, indent=1) if args.json else st.d["message"])
    return EXIT_UPDATE if avail_any else EXIT_OK


def cmd_apply(cfg, st, game, app, settings, args):
    wait_for_time(cfg, cfg.time_wait if args.auto else min(cfg.time_wait, 15))
    refresh_installed(st, game, app)
    kw = dict(version=args.version, force=args.force, auto=args.auto, use_cache=not args.no_cache)
    if args.what == "all":
        return apply_both(st, game, app, kw)
    (game if args.what == "game" else app).apply(**kw)
    st.checked()
    st.save()
    print(st.d["message"])
    return EXIT_OK


def apply_both(st, game, app, kw):
    """the app first (a newer game may need a newer host), then the game"""
    kw = dict(kw, version=None)
    msgs, app_err = [], None
    try:
        app.apply(**kw)
        msgs.append(st.d["message"])
    except UpdateError as e:
        if e.code not in APP_QUIET:
            if e.code in NETWORK:
                raise
            app_err = e
            msgs.append("StickTime Pi: " + e.message)
    try:
        game.apply(**kw)
        msgs.append(st.d["message"])
    except UpdateError as e:
        e.message = " ".join(msgs + [e.message])
        raise
    st.checked()
    msg = " ".join(m for m in msgs if m)
    if app_err is not None:
        # the game went on; the app's failure still counts
        st.set("error", msg, 0, error=app_err.message)
        print(msg)
        return app_err.exit_code
    st.set("done", msg, 1, error="")
    print(msg)
    return EXIT_OK


def cmd_rollback(cfg, st, game, app, settings, args):
    (game if args.what == "game" else app).rollback()
    print(st.d["message"])
    return EXIT_OK


def cmd_boot(cfg, st, game, app, settings, args, network=True):
    st.set("installing", "Checking the installed versions…", 0.05, error="")
    notes = []
    for part in (app, game):
        try:
            notes += part.repair()
        except UpdateError as e:
            notes.append(e.message)
        except OSError as e:
            err = fs_error(e)
            if err is None:
                raise
            notes.append(err.message)
            break
    refresh_installed(st, game, app)
    msg = ("After an interrupted update: %s." % "; ".join(notes)) if notes else ""
    st.set("idle", msg, 0, error="")
    if notes:
        log(msg)
    if not network or getattr(args, "no_network", False) or not truthy(settings.get("auto_update", "off")):
        print(msg or "Nothing to repair.")
        return EXIT_OK
    wait_for_time(cfg, cfg.time_wait)
    try:
        apply_both(st, game, app, dict(version=None, force=False, auto=True, use_cache=False))
    except UpdateError as e:
        if e.code in NETWORK:
            # offline at boot is normal (hotspot mode): say so, keep the installed versions, no error badge
            st.set("idle", (msg + " " if msg else "") + "No automatic update: " + e.message, 0, error="")
        else:
            st.failed(e)
        print(e.message)
    return EXIT_OK


def cmd_repair(cfg, st, game, app, settings, args):
    return cmd_boot(cfg, st, game, app, settings, args, network=False)


COMMANDS = {"check": cmd_check, "apply": cmd_apply, "rollback": cmd_rollback, "boot": cmd_boot, "repair": cmd_repair}
