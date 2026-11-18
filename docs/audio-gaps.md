# Audio gaps: silent ringtone previews, Safari audio that spins

Analysis and spec only (Opus, 2026-11-18). No source changed, nothing built or run. Inputs: HANDOFF-2026-11-16.md,
HANDOFF-2026-11-17-night.md (the hands-on BT session), docs/bluetooth-audio.md, ios165/{select-system.py,
rootfs.slt, rootfs/}, the dyld cache strings (ios165/20F66__iPhone10,3_6), userland/{build.sh, mkjobs.py,
root/LaunchDaemons, root/etc/inject.conf}, userland/src/{init.c, inject.c, inject_media.c, sound_test.c, tolog.c,
rwtouch.c}, xnu/files/bsd/kern/kern_minimac.c, logs/bt-hands1.log. **GUESS** marks anything not read in code,
a plist, a string table or a log.

**Read this first: there is a confound.** Both symptoms were reported from the hands-on BT session *after a
speaker was paired* ("All audio silent after pairing ... Not tested: audio BEFORE pairing", HANDOFF-11-17-night).
docs/bluetooth-audio.md H1 (a route change to a Bluetooth device that has no HAL plug-in) can by itself
silence previews and stall Safari's player. Every check below runs on a **non-BT image, with nothing paired**.

---------------------------------------------------------------------------------------------------------------

## (a) Ringtone previews in Settings > Sounds & Haptics

### What the preview path needs (from the plists and the cache)
| Piece | Where | State in this port |
|---|---|---|
| The list of tones | `ToneKit.framework/TKRingtones.plist` (ids `system:Opening`, `system:Radar`, ...). `<default>` is `system:Opening` (string in the cache) | Present. That is why the list shows even though the files are missing. |
| The tone files | ToneLibrary maps `system:NAME` to `/Library/Ringtones/NAME.m4r`. The string `/Library/Ringtones` is in the cache | **Missing.** `select-system.py` SKIP_PREFIX has `Library/Ringtones/`, and the system image has no `/Library/Ringtones` directory at all (ios165/rootfs/Library: Application Support, Audio, LaunchDaemons, RegionFeatures, Wallpaper). |
| Alert and text tones | `ToneLibrary.framework/AlertTones/Modern/*.m4r` (Note, Chord, ...: 732 KB), `System/Library/Audio/UISounds/**` (21 MB), `ToneLibrary.framework/AlarmWakeUpRingtones` | Present. |
| Playback code | ToneLibrary `TLAlertQueuePlayerController` uses **AVQueuePlayer** (string `@"AVQueuePlayer"`, `_startPlaybackForStateDescriptor:usingConfirmedPlayableAsset:`). `TLAlertSystemSoundController` uses `AudioServicesPlaySystemSoundWithOptions` (mediaserverd's SystemSoundServer) | Code is in the cache. Which back end a *preview* uses is a **GUESS**: queue player for ringtones. |
| Daemons | mediaserverd (AudioSession, SystemSoundServer, coremedia.*). `com.apple.tonelibraryd.xpc` is a framework XPC service, present in ToneLibrary.framework/XPCServices | mediaserverd is loaded (MORE_JOBS). tonelibraryd is launched on demand like other framework XPC services (**GUESS** that it is not needed for a preview at all). |

Also missing for the same reason: Clock's alarm sounds. The default alarm tone, `Radar`, is in /Library/Ringtones
too (**GUESS** that iOS 16 still uses Radar as the default alarm).

**Is the missing files the whole cause?** Missing files are certainly *a* cause, because every `system:` ringtone,
including the fallback default `Opening`, points at a path that does not exist. They are the *whole* cause only if
the AVQueuePlayer path works on this port. Nothing in the repo has ever played through AVPlayer: sound_test uses
an AudioQueue and system sounds. That is the same unknown as (b). Two checks settle it:
- Hands-on, free, no build: Settings > Sounds & Haptics > **Text Tone** > tap **Note**. Its file is in the image.
  Do it before pairing anything. If Note plays, the ringtone files are the only gap. If it is silent too, it
  shares the playback problem of (b). (**GUESS** that text-tone previews use the same queue-player back end.
  If Note plays but the D1 `avplayer` step below fails, the text tone went through system sounds.)
- Unattended: run D1 below. Its step 1 plays `AlertTones/Modern/Note.m4r` through AVPlayer.

### Where to get the files: this Mac, 0 bytes downloaded
macOS has the same tones at
`/System/Library/PrivateFrameworks/ToneLibrary.framework/Versions/A/Resources/Ringtones/`. All 53 names of
iOS 16.5's /Library/Ringtones are there. Sizes compared with rootfs.slt:
- **28 are byte-for-byte the same size**: all of the "modern" set and Opening and Reflection. Opening is
  267,506 B on both. 12.1 MB together.
- The 25 "classic" tones (Alarm, Bark, Marimba, ...) are slightly different encodes, within a few percent.
  They are the same sounds.
- All 53 iOS files total 18.0 MB.

The IPSW is not a cheap source. The tones are inside rootfs.dmg (4.7 GB). fetch-from-ipsw.py streams whole zip
members only, and the hdiutil-over-HTTP range trick (bluetooth-audio.md B1) is still untested. Use it only if a
non-Mac build host needs them; then put them in `../ios-extra/Ringtones/` as the fallback source below.

### Minimal useful subset
| Set | Files | Bytes | Note |
|---|---|---|---|
| Minimum | Opening | 267,506 | The default ringtone, and the tone ToneLibrary falls back to (**GUESS** about the fallback). |
| **Recommended (default)** | Opening, Radar, Hillside, Illuminate, Stargaze, Beacon | **508,919** | The default, the alarm default (Radar) and the four smallest modern tones, so there is something to choose from. All six have the same size on macOS and iOS. |
| Full modern | the 28 same-size files | 12.1 MB | Only if the RAM cost is acceptable: root.apfs lives in RAM, and AAC does not compress. |

Tones that are not shipped still appear in the list and stay silent, or play the fallback (**GUESS**). That is
acceptable.

### Cheapest way to ship (no SYSTEM=1, no var store)
Put them in the small image and bind them over the path iOS hard-codes. This uses the same pattern as B3
(init.c:1421), which is already implemented for the BT HAL. Test images and stay images get them the same way,
and the var store is not involved.

**R1. `userland/build.sh`**, after `root/` is populated and before the mkapfs line (~594):
```bash
# ringtones (docs/audio-gaps.md): /Library/Ringtones is not in the system image; init binds root/ringtones there
RINGTONES=${RINGTONES-"Opening Radar Hillside Illuminate Stargaze Beacon"}   # RINGTONES= for none
for d in ../ios-extra/Ringtones /System/Library/PrivateFrameworks/ToneLibrary.framework/Versions/A/Resources/Ringtones; do
	[ -n "$RINGTONES" ] && [ -d "$d" ] || continue
	mkdir -p root/ringtones
	for t in $RINGTONES; do cp "$d/$t.m4r" root/ringtones/ && chmod 644 "root/ringtones/$t.m4r"; done
	echo "  ringtones: $(ls root/ringtones | wc -l | tr -d ' ') from $d ($(du -sk root/ringtones | cut -f1) KB)"
	break
done
```
Names must stay exactly `NAME.m4r` (spaces included, e.g. `Slow Rise.m4r`). Quote them if any are added.

**R2. `userland/src/init.c`**, next to the B3 block (after line ~1423, before `run_rc`). Reuse `bt_st`:
```c
	/* ringtones (docs/audio-gaps.md): ToneLibrary reads /Library/Ringtones, which the system image does not have */
	if (!flash_root_mode && stat(in_image("/ringtones/Opening.m4r"), &bt_st) == 0 && stat("/Library/Ringtones", &bt_st) != 0) {
		check(synthetic_dir("/Library/Ringtones") && bind_dir(in_image("/ringtones"), "/Library/Ringtones"),
		    "ringtones on /Library/Ringtones");
	}
```
`synthetic_dir` (kern.apfs_synthetic, apfs_vfsops.c:647) creates the missing mount point on the read-only APFS
root. Its parent `/Library` exists. The flash_root mode is left alone: Apple's own volume there would have the
directory.

**R3. Checks.**
- Unattended: the init line `ringtones on /Library/Ringtones` is ok. `sound_test avplayer /Library/Ringtones/Opening.m4r 6`
  (the step from D1) reaches `status 1` and `playing`, and the kernel logs `redwood-audio: ~6 s written ...
  of it sound`.
- Hands-on: Ringtone > tap Opening and Radar. Clock > alarm > Sound > Radar.
- Budget: root.apfs grows by ~0.5 MB. The payload is ~8.5 MB against the loader's 64 MB. Check that
  `xl_x6826b build` still succeeds.

---------------------------------------------------------------------------------------------------------------

## (b) Safari: "play" spins for ever, no sound

### What web media playback needs on iOS 16.5, and what the port has
| Need | Port | Evidence |
|---|---|---|
| WebKit GPU process (`cryptex/.../WebKit.framework/XPCServices/com.apple.WebKit.GPU.xpc`). It hosts `MediaPlayerPrivateAVFoundationObjC` (AVPlayer) and Web Audio's output unit | Launches. libredwood is injected (inject.c:3630 only fixes Metal shaders, nothing about media) | It draws pages. |
| Media bytes: WebKit loads them itself. AVURLAsset gets `AVURLAssetRequiresCustomURLLoadingKey` / `WebCoreNSURLSession` / `RemoteMediaResourceManager_*` (strings in cache .07): GPU process -> WebContent -> **Networking process** | Works for pages. In bt-hands1.log the only TCP is `com.apple.WebKit:98` and MobileSafari, not mediaserverd. So mediaserverd does no HTTP of its own, and nsurlsessiond is not involved. | logs/bt-hands1.log. The HANDOFF also says pages kept loading. **Network is the least likely cause.** |
| mediaserverd: AudioSession, AURemoteIOServer, `com.apple.coremedia.player.xpc` (remote FigPlayer), `formatreader.xpc`, `customurlloader.xpc`, systemcontroller, decompressionsession | Loaded (MORE_JOBS), with inject_media.c (camera, flashlight, a `_os_log_impl` filter). Built-in output works through AudioQueue and system sounds (U1/btU1b) | **AVPlayer has never been exercised on this port.** Whether an app's AVPlayer runs remote in mediaserverd or in process on iOS 16 is a **GUESS** either way. |
| **mediaparserd** (`/usr/libexec/mediaparserd`, job `com.apple.mediaparserd`, services `com.apple.coremedia.mediaparserd.{formatreader,manifold,playlistfileparser,streamplaylistparser,mutablemovie,utilities}`) | **Binary in the image, job NOT loaded.** It is not in IOS_JOBS or the HANDOFF MORE_JOBS, and build.sh never passes REST=1, so root/LaunchDaemons has 42 plists and this is not one of them | FeatureFlags/Domain/CoreMedia.plist: `UseMediaparserd_FileParsing_OptIn` = true, `UseMediaparserd_Shoutcast` = true, HLS ones true. MediaToolbox exports `FigRemote_ShouldConnectToMediaparserdForFileParsing`. **GUESS** that WebKit or AVFoundation opts in for web content. |
| mediaplaybackd | Not in iOS 16.5. No such job or string, only `mediaplaybackcore` | Not a gap. |
| Audio/video decoders | Audio codecs (MP3, AAC) are in the dyld cache (AudioToolbox/AudioCodecs). No .component bundles are needed. VideoToolbox decodes in software in mediaserverd (HEIC already works) | Not a gap for audio-only pages (**GUESS** for exotic codecs such as Opus/WebM). |
| Sandbox extensions | kern_minimac.c: every Sandbox request is "allowed". Extension requests (5, 7, ...) are answered, and pages, which also need extensions, work | Unlikely. |
| FairPlay (fairplayd) | Not loaded | Only for DRM/EME. Sound-effect sites do not use it. |

### Candidate causes, ranked
| # | Cause | Why | What tells it apart (unattended unless noted) |
|---|---|---|---|
| C1 | **BT confound.** The test was done with a speaker paired and no BTAudioHALPlugin. The route or IO start waits for a device that never comes (bluetooth-audio.md H1) | The only Safari attempt was after pairing. During the attempts the kernel shows no `redwood-audio: ... written` at all; the only AFE activity is the lock sound at 2452/2461 s | D1 on a **non-BT** image. If D1's `avplayer` steps and the Safari step play, C1 was the whole story, and the rest of (b) is the BT work in bluetooth-audio.md. Hands-on: try the same site before pairing. |
| C2 | **mediaparserd missing.** AVAsset/FigPlayer file or stream parsing is sent to `com.apple.coremedia.mediaparserd.formatreader.xpc`. The lookup fails, the item never becomes ready or fails, and the site waits for `canplay` forever (sites rarely handle `error`). Spinning on an error is a site behaviour (**GUESS**) | Feature flags on, binary present, job absent. Zero-cost fix | A/B: D1 (without) vs D2 (`com.apple.mediaparserd` added). Signs in D2: launchd `Successfully spawned mediaparserd[N] because ipc`, and `avplayer` reaches `status 1` where D1 did not. |
| C3 | **The AVPlayer pipeline in mediaserverd blocks** (remote player or system controller). Compare the Infinix: "SpringBoard's synchronous request ... FigSystemControllerRemoteCreate is never answered", mkjobs.py / run 253 | Never tested. AudioQueue working does not prove FigPlayer works | D1 `avplayer` lines stop changing (status 0, timeControl 1 "waiting") with no error. Then `media_threads=SECONDS` (inject_media.c) dumps mediaserverd's threads; name them with tools/cache-name.py. Also check that the mediaserverd pid is stable in `ps -A`. |
| C4 | **Problem specific to the GPU process.** It dies or asserts on media init (libredwood injected, Metal/GL hooks), or the custom-URL-loading relay stalls | AVPlayer would work in sound_test but not in Safari | D1 `avplayer` file + https pass, but the D1 Safari step stays silent. Look for launchd `com.apple.WebKit.GPU ... exited`/crash lines, and the GPU pid changing between the two `ps -A`. Next: spec S2 below. |
| C5 | Audio session or ringer: sound_test's session is SoloAmbient by default and obeys the ringer | Would make sound_test silent, not Safari spin | Handled: the `avplayer` step sets category Playback first and prints `setActive`. |
| C6 | Network, ATS, certificates | Pages load. The media goes through the same Networking process | Only if the `https` step fails with an NSURLErrorDomain code while the file step passes. |

### D1: the first diagnostic run (Sonnet-implementable)
Two small code changes, a new rc file, then two runs (D1, D2) with the **default non-BT** build command from
HANDOFF-2026-11-16.md. Do not use var_store or emmc_write. Tell the user before the runs start.

**S1. `userland/src/sound_test.c`: new step `avplayer URL SECONDS`.** It is already linked with AVFoundation
(build.sh:211). Use the existing `send()`/`text()` style. URL is a path (no `://`) or an http(s) URL.
```c
/* AVPlayer, as an application or WebKit's GPU process would play a file or a URL: what the item and the player say, every second */
static int
avplayer(const char *where, double seconds)
{
	CFStringRef s = CFStringCreateWithCString(NULL, where, kCFStringEncodingUTF8);
	Class NSURL = objc_getClass("NSURL");
	id url = strstr(where, "://") ? ((id (*)(id, SEL, id))objc_msgSend)((id)NSURL, sel_registerName("URLWithString:"), (id)s)
	    : ((id (*)(id, SEL, id))objc_msgSend)((id)NSURL, sel_registerName("fileURLWithPath:"), (id)s);
	id session = send((id)objc_getClass("AVAudioSession"), "sharedInstance");
	id error = NULL;
	BOOL ok;

	printf("sound_test: avplayer %s: category Playback\n", where);
	((BOOL (*)(id, SEL, id, id *))objc_msgSend)(session, sel_registerName("setCategory:error:"), (id)CFSTR("AVAudioSessionCategoryPlayback"), &error);
	printf("sound_test: avplayer: setActive...\n");          /* a hang here = the audio session (mediaserverd) */
	ok = ((BOOL (*)(id, SEL, BOOL, id *))objc_msgSend)(session, sel_registerName("setActive:error:"), YES, &error);
	printf("sound_test: avplayer: setActive %d %s\n", ok, error ? text(send(error, "description")) : "");
	id player = ((id (*)(id, SEL, id))objc_msgSend)((id)objc_getClass("AVPlayer"), sel_registerName("playerWithURL:"), url);
	printf("sound_test: avplayer: player %p, play\n", (void *)player);
	((void (*)(id, SEL))objc_msgSend)(player, sel_registerName("play"));
	long last = -1;
	for (double t = 0; t < seconds; t += 1) {
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1, false);
		id item = send(player, "currentItem");
		long status = item ? ((long (*)(id, SEL))objc_msgSend)(item, sel_registerName("status")) : -1;
		long control = ((long (*)(id, SEL))objc_msgSend)(player, sel_registerName("timeControlStatus"));
		struct { int64_t value; int32_t timescale; uint32_t flags; int64_t epoch; } now =
		    ((typeof(now) (*)(id, SEL))objc_msgSend)(player, sel_registerName("currentTime"));
		id failure = item ? send(item, "error") : NULL;
		/* status 0 unknown, 1 ready, 2 failed; control 0 paused, 1 waiting, 2 playing */
		if (status * 10 + control != last || (int)t % 5 == 0) {
			printf("sound_test: avplayer t=%.0f status %ld control %ld at %.2f s waiting \"%s\" %s\n", t, status, control,
			    now.timescale ? (double)now.value / now.timescale : -1.0, text(send(player, "reasonForWaitingToPlay")),
			    failure ? text(send(failure, "description")) : "");
			last = status * 10 + control;
		}
	}
	((void (*)(id, SEL))objc_msgSend)(player, sel_registerName("pause"));
	CFRelease(s);
	return last != 12;      /* passed = ready and playing at the end */
}
```
Add to `main()`: `else if (strcmp(argv[i], "avplayer") == 0 && i + 2 < argc) { failed |= avplayer(argv[i + 1], atof(argv[i + 2])); i += 2; }`,
and a usage line in the header comment. `text()` already copes with nil. Keep the error description to
`%.200s` if the lines are too long for `kern.redwood.say`.

**S1b. `userland/rc.jobs.media`** (new, used as `RC_JOBS=rc.jobs.media`). Before the build, check from the Mac
that the two URLs answer: `curl -sI URL | head -1` (a few bytes only). If one does not, replace it with any
small public mp3, keeping one plain-http URL and one https URL.
```
# Commands run by the launchd job com.redwood.demo. docs/audio-gaps.md D1: AVPlayer on a local file, plain http and https, then Safari on an mp3.
/redwood/bin/tolog OS_ACTIVITY_DT_MODE=YES /redwood/bin/sound_test springboard route avplayer /System/Library/PrivateFrameworks/ToneLibrary.framework/AlertTones/Modern/Note.m4r 8 avplayer http://commondatastorage.googleapis.com/codeskulptor-demos/DDR_assets/Kangaroo_MusiQue_-_The_Neverwritten_Role_Playing_Game.mp3 20 avplayer https://www.w3schools.com/html/horse.mp3 15 route
/bin/ps -A
/redwood/bin/rwtouch say safari notify com.redwood.unlock wait 5
/redwood/bin/battery_test open https://www.w3schools.com/html/horse.mp3
/redwood/bin/rwtouch wait 12 say tap tap 0.5 0.5 wait 20 say end
/bin/ps -A
```
If tolog's `NAME=VALUE` makes the output too long for the 64 KB kept zone (check tolog.c; `TOLOG_PLAIN=1` sends
it to the console once instead of keeping it), drop `OS_ACTIVITY_DT_MODE=YES` from the line. The console tail
still has everything for a 300 s run. Safari's media document tap position (0.5 0.5) is a **GUESS**. The
screenshots show whether the player is there.

**S1c. Runs.** Use the default build from HANDOFF-2026-11-16.md line 120 plus `RC_JOBS=rc.jobs.media
OSLOG="com.apple.backboardd com.apple.mediaserverd"`:
- **D1**: as is. `SHOTS="fbshot=5 fbshot_every=10 fbshot_w=70" tools/r1-auto.sh md1 "<BASE> settings=1 media_threads=150" 300`.
  (`settings=1` only keeps r1-auto from adding autotest=1. Use any no-scenario word r1-auto.sh:19 accepts.)
- **D2**: same with `com.apple.mediaparserd` appended to MORE_JOBS. Name `md2`.
- Afterwards rebuild the default userland, as the BT agent did.

**Reading D1/D2.** Grep `!sound_test: avplayer`, `redwood-audio:`, `redwood: audio: playing`, `mediaparserd`,
`WebKit.GPU`, `!media t`, `auto: safari|tap|end`.
| Result | Meaning | Next |
|---|---|---|
| D1 file, http and https all reach `status 1 control 2` with time advancing, plus `redwood-audio: ... s of it sound`; the Safari tap also gives sound | C1: the hands-on failure was the BT route | Nothing to fix in (b); continue bluetooth-audio.md |
| D1 avplayer steps stuck or failed, D2 pass | **C2** | Fix F1 |
| Both stuck at `status 0 control 1` with no error | C3 | Read the `!media t` frames (tools/cache-name.py). Check the mediaserverd pid. Name the blocking service, then write a new spec |
| `setActive...` with no `setActive N` line | The audio session hangs in mediaserverd (C3 variant) | Same as above |
| Status 2 with an error | Read the domain and code: `AVFoundationErrorDomain -11800` plus underlying `-12xxx` (CoreMedia) means parsing or decoding; `NSURLErrorDomain` means C6 | Spec per code |
| avplayer steps pass, Safari silent / GPU pid changes / crash lines | C4 | Spec S2 |

### F1: the most likely fix (if D2 confirms C2)
`userland/build.sh`, after the MORE_JOBS line (~137). Whenever mediaserverd is loaded, load its parser daemon
too. It has no RunAtLoad or KeepAlive, so launchd starts it only when a service of it is looked up, and it costs
nothing when unused:
```bash
# mediaparserd: CoreMedia parses media files and streams in it (FeatureFlags CoreMedia UseMediaparserd_*);
# without it AVPlayer items never get ready (docs/audio-gaps.md C2). Started on demand only.
case " $(echo $IOS_JOBS) " in *" com.apple.mediaserverd "*) IOS_JOBS="$IOS_JOBS com.apple.mediaparserd" ;; esac
```
mkjobs.py prints `job com.apple.mediaparserd: /usr/libexec/mediaparserd` and must not print "NOT in the system
image". Verify with D2's rc file (the `avplayer` lines pass) and then hands-on (a sound-effect site, before
pairing). Update the stay-image notes in the HANDOFF: nothing to add, because the build picks it up.

### S2 (only if C4): GPU-process log
Add `com.apple.WebKit.GPU` to the `libredwood-media.dylib` INJECT line. In inject_media.c, make
`in_mediaserverd` a `log_filter` flag that is also set for `com.apple.WebKit.GPU` when the boot-arg
`media_log=1` is present. For that process, widen the format filter to `"rror"`, `"ailed"`, `"MediaPlayer"`,
`"AVF"`, `"Fig"`, `"mediaparserd"`, `"RemoteMediaResource"`, capped at 60 lines and prefixed `gpu log:`. The
`__interpose` already works for mediaserverd (U1 matched route-change lines). Rerun D1's Safari part with
`media_log=1`.

---------------------------------------------------------------------------------------------------------------

## Order of work
1. User, next hands-on, before pairing anything: Text Tone > Note, then one sound-effect site in Safari.
   This costs no build. It answers C1 and the "whole cause" question of (a) directly.
2. R1-R3 (ringtones): independent of everything else, ~25 lines, safe for test images.
3. S1/S1b + D1/D2 (one build, two 300 s runs). Then F1, or the follow-up named in the table.

Remaining unknowns, about 5: which back end previews use (queue player vs system sound); whether AVPlayer runs
remote in mediaserverd; whether anything opts into mediaparserd for files (the C2 premise); the Safari
media-document tap position; and whether OS_ACTIVITY_DT_MODE output fits the kept log.
