# OHFLOCK: the corridor

`build/ohflock` is a showcase you watch. Twenty-four small unarmed aircraft, the kites, hold an air corridor
open for a relief convoy crossing a desert basin at night. They face a conventional air defence: two pairs of
fighters with infrared and radar missiles and cannon, a SAM battery, three AAA guns cued by acoustic arrays,
two strike jets with laser-guided bombs, a GNSS jammer, and a reconnaissance satellite overhead. Hundreds of
rounds and missiles are fired at the flock. The flock fires nothing, and the convoy gets through.

```bash
./build/ohflock --html build/ohflock.html          # the 3D replay: open it in a browser
./build/ohflock                                    # the same battle, narrated in the terminal
./build/ohflock --no-satlink                       # the same battle without satellite tracking aboard
./build/ohflock --script examples/ohflock/orders.txt
./build/ohflock --serve 1987                       # live; drive it from gyde (below)
./build/ohflock --selfcheck                        # determinism and outcome checks, a ctest entry
```

A prebuilt replay, with the orders in `examples/ohflock/orders.txt`, is in [examples/ohflock/ohflock.html](../examples/ohflock/ohflock.html).

## What the flock does instead of shooting

- **Light, on sensors only.** Each kite carries a low-power RGB laser. It dazzles *sensors*: the SAM battery's
  electro-optical tracker, the gunners' sights, the strike jets' targeting pods. A dazzled tracker loses its
  target, and a missile guided by it misses. An interlock keeps every beam off cockpits and people (the pods are
  below the cockpit line), and nobody in the scenario is hurt. Kites also paint decoy spots on empty sand, and a
  laser-guided bomb looking for a spot follows one there. Lasers heat up and need to cool, so the flock spends them
  where they matter most.
- **Thrown sound.** The guns are cued by acoustic arrays that listen for the loudest aircraft. Kites project
  phantom sound sources into empty sky, louder than their own engines, and the guns fire at the phantoms.
- **Knowing where the danger is.** Radar-guided missiles are beaten by flying across their line of sight: a slow
  target with no closing speed hides in the radar's clutter notch. Infrared missiles are beaten by a break turn at
  the moment the shared track predicts. The SAM's position is known, so one kite stands picket within laser range of
  its tracker before the first launch.
- **SatLink.** The ground has GNSS jammed from T+75 s. The flock navigates by the Doppler shift of beacons from
  SAHAB, a 60-satellite relay shell, whose motion SatLink's SGP4 predicts exactly: a maximum-a-posteriori fix of
  position and inertial velocity bias from each window of measurements, with each measurement used once and
  the covariance carried forward (the idea behind Transit, the first satellite navigation system). Thrown sound and
  decoy spots are only useful if the flock knows where it is; with this it keeps its error to tens of metres. SatLink
  also predicts when OKULAR-2, the other side's imaging satellite, will be overhead, so the flock disperses and
  descends before it rises, and it predicts relay windows, so orders from the ground are sent when a relay is up.

## What it shows

With the default seed, the opposing force fires 244 shots and missiles at the flock and convoy. 136 chase thrown
sound, 65 lose their tracker or sight to a laser, 25 radar missiles lose their target in the notch, 9 infrared
missiles are beaten by a predicted break turn, and all 8 laser-guided bombs follow decoy spots into empty sand.
One kite is lost, the convoy is not touched, the flock fires nothing, and under jamming its navigation error never
exceeds 43 m.

`--no-satlink` runs the same battle with no satellite tracking aboard. The inertial navigation drifts to about
800 m, so thrown sound and decoy spots land in the wrong places. The imaging satellite photographs a tight
formation, which gives the guns better cues, and orders sent from the ground are lost when no relay is overhead.
On the default seed the flock still gets the convoy through, losing two kites; over twelve seeds it takes 18 hits
with SatLink and 40 without, and loses 18 kites against 38. `--selfcheck` runs and prints that comparison.

## Directing the flock

It is mainly a demonstration to watch, but a human can direct it. Orders travel up through a SAHAB relay with a
20 degree link mask, so an order sent while none is overhead waits for the next one, and SatLink says when.

| Order | Effect |
| --- | --- |
| `posture screen\|tight\|wide` | how far out the sections hold their stations |
| `lasers on\|off` | whether lasers are used at all |
| `acoustics on\|off` | whether sound is thrown |
| `corridor east\|west\|centre` | shift the screen 1.5 km off the road |

Live with gyde:

```
$ ./build/ohflock --serve 1987 --speed 4
$ gyde --url http://127.0.0.1:1987
> tool flock.status          the battle at a glance
> tool flock.sky             satellites over the corridor, relay links and Doppler (SatLink)
> tool flock.explain         the flock's recent decisions and why
> tool flock.order posture wide
```

`ohflock --serve` runs an embedded Gygax service on loopback with the `flock.*` tools registered, so anything that
speaks the service API can drive it, gyde included. `--script FILE` takes `<seconds> <order>` lines instead;
`examples/ohflock/orders.txt` sends one order inside a relay window and one outside it.

## The replay

`--html FILE` writes a self-contained page with no external files: a moonlit dune field drawn with its contour
lines, the flock in section colours, the opposing aircraft and ground units, missile and tracer trails ending in
how each shot ended, laser beams, thrown-sound rings, and the real sky. The SAHAB relays and OKULAR-2 are drawn
where SatLink puts them, and a sky dome shows relay links and their beacon Doppler. Counters, outcome bars,
navigation state and the narrative follow the battle. Drag to orbit, scroll to zoom, space to pause; 1 to 5
choose the camera (director, overview, convoy, SAM, chase a kite).

## What is real and what is not

Everything is fictional: the place names (the Sahran basin at 20.4 N 49.1 E), the forces, and the SAHAB and
OKULAR-2 satellites, which are invented element sets on realistic orbits, propagated by SatLink. The battle
begins five minutes before OKULAR-2's first high pass over the corridor, so its timing comes from orbital
mechanics rather than a script.

The orbits, sky geometry, Doppler and the navigation filter are computed for real. The engagement model is not.
Motion is kinematic, and every shot's outcome is a probability that depends on what the flock did about it:
notching, a break turn, a laser on the right sensor, a phantom closer to the array than any kite, a decoy spot
placed accurately enough. It is meant to show how intelligence, light, sound and satellite awareness work
together, not to predict how any real system performs. Same seed and same orders give the same battle, which
`--selfcheck` verifies with the outcome checks above.
