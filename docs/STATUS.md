# Rabbit r1 iOS 16 port: status

Last updated: 2026-11-18 12:40 PST. Kept current as work lands; details are in the other files under `docs/`.

"Hands-on" means confirmed by a person using the device. "Unattended" means it passed an automated run but nobody has seen it yet.

## Done (hands-on)

- Screen and touch, including rotation
- Side button and scroll wheel
- Lock and wake; correct clock (RTC)
- Brightness (Control Centre slider)
- Battery gauge (holds after unplugging)
- Vibration
- Camera: correct colours, flip motor, front and back previews upright
- Built-in speaker audio
- Wi-Fi, including alongside a paired Bluetooth speaker
- Bluetooth pairing from Settings; reconnects after the speaker is power-cycled
- CPU speed governor (no lag)

## Done (unattended only, needs a hands-on check)

- Settings app opens (Wallet row hidden, CoreImage guard)
- Saved photos carry the orientation tag
- Tone playback through AVPlayer (text tone, ringtone) with nothing paired
- Six ringtones shipped by default (`/Library/Ringtones` bound at boot)
- Cellular in test images: LTE registration, data bearer, DNS and HTTP over `pdp_ip0` (8 of 10 approved radio-on attempts used)
- Modem as a service with the radio off (`r1_md=10`): 30 minutes, no faults, clean shutdown
- Bluetooth audio plug-in loads in mediaserverd and connects to bluetoothd
- Boot image has about 1.4 MB free (Android ramdisk repacked lz4 to gzip)
- Cellular stage 4c/4d on a simulated bearer (radio off): `celld` publishes the service, `pdp_ip0` becomes the primary interface, DNS and traffic go out on it, the data budget cuts traffic off (about 5 KB overshoot)

## In progress

- Using Open Mapple (github.com/open-mapple, same code lineage, MediaTek phone): plan in `open-mapple-plan.md`; phase 1 small items done (volume guard, UI-activity CPU boost with no measured gain yet, boot-args length check); pass benchmark and profiler next
- Microphone: spec written (`microphone-spec.md`), implementation waiting for the owner's OK to change the audio driver
- Microphone and video recording: analysis of what each needs (see open problems)
- Bluetooth speaker audio (A2DP): ready for a hands-on session; no sound has been sent to a speaker yet

## Open problems

- Microphone records silence: the kernel audio driver has no capture (uplink) path at all
- Video recording fails in two places: Camera's capture graph loops as soon as Video mode is selected (before any encoder is asked for), and neither of Apple's dormant software encoders (H.264, HEVC) will create a session when registered (-12908)
- Cellular alongside Wi-Fi (Wi-Fi should win) is untested: unattended runs have no Wi-Fi
- All audio went silent after a speaker was paired (likely the route moved to a Bluetooth output that could not carry sound; not confirmed)
- Safari audio spins and never plays; a Safari file download would not start (untested unattended: no Wi-Fi in those runs)
- Home-screen swipes run at about 8 to 13 fps (parked)
- Camera frames sometimes painted twice after a home swipe (parked)
- Side-button exit and radio-on shutdown of the modem service not yet exercised

## Not yet done

- Everyday cellular in a normal image: radio-on service (`r1_md=11`, never run), status bar, airplane toggle (stages 4e/4f; needs the remaining decisions in `cellular-stage4.md`)
- Bluetooth on the saved data store (needs the keybag permanently, and a backup first)
- Phone calls and SMS (not built; current safety rules forbid dialling and SMS)
- Bluetooth call audio (HFP)
- USB while in iOS
- Sleep / low-power idle

## Dropped or nothing to gain

- GPS
- 8th CPU core
- Memory speed scaling (DRAM already runs at 3200 MT/s)

## Waiting on a person

1. Hands-on Bluetooth session: speaker audio, Safari audio, ringtone previews, before and after pairing
2. Stay-in-iOS session on saved data: confirm Settings and photo orientation
3. Cellular everyday-behaviour decisions before the last two radio-on attempts
4. Bluetooth on saved data: throw-away store only, or permanent with a backup
