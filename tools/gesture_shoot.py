"""
gesture_shoot.py -- Python + MediaPipe Hands gesture input for the game.

Detects up to two hands. SHOOT and WALK_FORWARD are now single-hand
poses (either hand, checked independently) -- LOOK_LEFT/LOOK_RIGHT are
the only ones still tied to a specific hand:

  - Any hand as a closed fist (thumb tucked in too), raised up -> SHOOT
  - Any hand as a thumbs-up (other 4 fingers curled, thumb out),
    raised up                                          -> WALK_FORWARD
  - Only the LEFT hand, flat open palm, raised up       -> LOOK_LEFT
  - Only the RIGHT hand, flat open palm, raised up      -> LOOK_RIGHT

Fist and thumbs-up share the same "other 4 fingers curled" base shape
and are told apart purely by thumb state (tucked vs. out) -- this
matters because the old is_fist() ignored the thumb entirely, which
would've made every thumbs-up also register as a fist. That's fixed
below (is_fist() now explicitly requires a tucked thumb too).

"Raised up" is approximated as "wrist is in the upper portion of the
camera frame" (RAISED_Y_THRESHOLD below) -- MediaPipe Hands only gives
hand landmarks, no body/shoulder position, so there's no way to check
"above your shoulder" directly. First-guess threshold, tune it if it
triggers too easily (hands at rest already read as "raised") or too
reluctantly (arms fully up still doesn't register).

Sends tiny UDP packets at the Godot client's GestureInput autoload for
every frame each pose holds -- same "local input source" idea as a
mouse/keyboard, just a camera.

Usage:
    pip install opencv-python mediapipe
    python gesture_shoot.py
    (press 'q' in the preview window to quit)

NOTE on handedness: MediaPipe's Left/Right label is relative to the
mirrored preview (this script flips the frame for a natural selfie
view). Swap HANDEDNESS_FOR_LEFT/HANDEDNESS_FOR_RIGHT below if
LOOK_LEFT/LOOK_RIGHT come out backwards when you test.
"""

import argparse
import math
import socket

import cv2
import mediapipe as mp

UDP_IP = "127.0.0.1"
UDP_PORT = 5555

# Which MediaPipe handedness label counts as your left/right hand for
# LOOK_LEFT/LOOK_RIGHT. Swap these two strings if testing shows it's
# backwards for you.
HANDEDNESS_FOR_LEFT = "Left"
HANDEDNESS_FOR_RIGHT = "Right"
HANDEDNESS_MIN_CONFIDENCE = 0.6

# Landmark indices (MediaPipe Hands, 21 points per hand).
WRIST = 0
THUMB_TIP, THUMB_IP, THUMB_MCP = 4, 3, 2
INDEX_TIP, INDEX_PIP, INDEX_MCP = 8, 6, 5
MIDDLE_TIP, MIDDLE_PIP, MIDDLE_MCP = 12, 10, 9
RING_TIP, RING_PIP = 16, 14
PINKY_TIP, PINKY_PIP = 20, 18

# How much farther the tip must be from the wrist than the pip joint
# to count as "extended" -- distance-based (and 3D, including
# MediaPipe's estimated depth z) rather than a pure y-compare, so it
# still works with the hand tilted/rotated toward the camera.
EXTEND_RATIO = 1.1

# Thumb tip distance from the index MCP, normalized by hand size, to
# count as "thumb out/spread" (part of the open-palm pose).
THUMB_SPREAD_RATIO = 0.5

# Wrist y-position (normalized, 0 = top of frame, 1 = bottom) below
# which a hand counts as "raised up". See module docstring -- this is
# a frame-position proxy, not a true height-relative-to-body check.
RAISED_Y_THRESHOLD = 0.5


def dist(a, b) -> float:
    # 3D distance (includes MediaPipe's estimated relative depth, z),
    # not just flat screen-space x/y -- keeps extension detection
    # working even when a hand isn't perfectly flat-on to the camera.
    return math.sqrt((a.x - b.x) ** 2 + (a.y - b.y) ** 2 + (a.z - b.z) ** 2)


def is_extended(lm, tip, pip) -> bool:
    return dist(lm[WRIST], lm[tip]) > dist(lm[WRIST], lm[pip]) * EXTEND_RATIO


def is_raised(lm) -> bool:
    return lm[WRIST].y < RAISED_Y_THRESHOLD


def is_fist(lm) -> bool:
    """All four fingers curled in AND the thumb tucked (not spread
    out). The thumb check is what keeps this from also matching a
    thumbs-up pose -- both have the same four-fingers-curled base
    shape, so thumb state is the only thing telling them apart."""
    hand_size = dist(lm[WRIST], lm[MIDDLE_MCP])
    if hand_size < 1e-5:
        return False
    thumb_spread = dist(lm[THUMB_TIP], lm[INDEX_MCP]) / hand_size
    return (
        not is_extended(lm, INDEX_TIP, INDEX_PIP)
        and not is_extended(lm, MIDDLE_TIP, MIDDLE_PIP)
        and not is_extended(lm, RING_TIP, RING_PIP)
        and not is_extended(lm, PINKY_TIP, PINKY_PIP)
        and thumb_spread <= THUMB_SPREAD_RATIO
    )


def is_thumbs_up(lm) -> bool:
    """Same four-fingers-curled shape as is_fist(), but with the thumb
    spread out instead of tucked in. Doesn't separately verify the
    thumb points specifically upward (vs. out to the side) -- with the
    other four fingers curled, sticking the thumb out mostly only has
    one comfortable direction anyway (up), so this hasn't needed a
    stricter check. Flag it if testing says otherwise."""
    hand_size = dist(lm[WRIST], lm[MIDDLE_MCP])
    if hand_size < 1e-5:
        return False
    thumb_spread = dist(lm[THUMB_TIP], lm[INDEX_MCP]) / hand_size
    return (
        not is_extended(lm, INDEX_TIP, INDEX_PIP)
        and not is_extended(lm, MIDDLE_TIP, MIDDLE_PIP)
        and not is_extended(lm, RING_TIP, RING_PIP)
        and not is_extended(lm, PINKY_TIP, PINKY_PIP)
        and thumb_spread > THUMB_SPREAD_RATIO
    )


def is_open_palm(lm) -> bool:
    """All four fingers AND the thumb extended/spread -- the flat
    "stop sign" hand pose. NOTE: this doesn't verify the palm (vs. the
    back of the hand) is actually what's facing the camera -- reliably
    telling those apart needs a palm-normal estimate that MediaPipe
    Hands' 21 sparse landmarks don't cleanly support. In practice,
    raising an open hand toward a camera in front of you naturally
    means the palm faces it anyway, so this hasn't been a problem to
    check separately -- flag it if testing says otherwise."""
    hand_size = dist(lm[WRIST], lm[MIDDLE_MCP])
    if hand_size < 1e-5:
        return False
    thumb_spread = dist(lm[THUMB_TIP], lm[INDEX_MCP]) / hand_size
    return (
        is_extended(lm, INDEX_TIP, INDEX_PIP)
        and is_extended(lm, MIDDLE_TIP, MIDDLE_PIP)
        and is_extended(lm, RING_TIP, RING_PIP)
        and is_extended(lm, PINKY_TIP, PINKY_PIP)
        and thumb_spread > THUMB_SPREAD_RATIO
    )


def classify_frame(hands_info) -> set:
    """hands_info: list of (landmarks, handedness_label, handedness_score)
    for every hand detected this frame (0, 1, or 2 entries). SHOOT and
    WALK_FORWARD trigger from ANY one hand doing the pose -- they don't
    need both hands, unlike LOOK_LEFT/LOOK_RIGHT which are still tied
    to a specific hand."""
    active = set()

    for lm, label, score in hands_info:
        if not is_raised(lm):
            continue

        if is_fist(lm):
            active.add("SHOOT")
        elif is_thumbs_up(lm):
            active.add("WALK_FORWARD")
        elif is_open_palm(lm):
            if score < HANDEDNESS_MIN_CONFIDENCE:
                continue
            if label == HANDEDNESS_FOR_LEFT:
                active.add("LOOK_LEFT")
            elif label == HANDEDNESS_FOR_RIGHT:
                active.add("LOOK_RIGHT")

    return active


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--headless", action="store_true",
        help="No preview window -- for when Godot launches this in the background (G-toggle).",
    )
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    mp_hands = mp.solutions.hands
    mp_draw = mp.solutions.drawing_utils

    cap = cv2.VideoCapture(0)
    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'MJPG'))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
    if not cap.isOpened():
        print("Could not open webcam (index 0). Check camera permissions/index.")
        return

    actions_str = "SHOOT/LOOK_LEFT/LOOK_RIGHT/WALK_FORWARD"
    if args.headless:
        print(f"[gesture_shoot] headless, sending {actions_str} to {UDP_IP}:{UDP_PORT}.")
    else:
        print(f"Sending {actions_str} to {UDP_IP}:{UDP_PORT}. Press 'q' to quit.")

    try:
        with mp_hands.Hands(
            max_num_hands=2,
            min_detection_confidence=0.7,
            min_tracking_confidence=0.6,
        ) as hands:
            while cap.isOpened():
                ok, frame = cap.read()
                if not ok:
                    continue

                frame = cv2.flip(frame, 1)  # mirror, feels natural facing the camera
                rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                results = hands.process(rgb)

                hands_info = []
                if results.multi_hand_landmarks and results.multi_handedness:
                    for hand_landmarks, handedness in zip(
                        results.multi_hand_landmarks, results.multi_handedness
                    ):
                        if not args.headless:
                            mp_draw.draw_landmarks(frame, hand_landmarks, mp_hands.HAND_CONNECTIONS)
                        hands_info.append((
                            hand_landmarks.landmark,
                            handedness.classification[0].label,
                            handedness.classification[0].score,
                        ))

                active = classify_frame(hands_info)

                for action in active:
                    sock.sendto(action.encode("ascii"), (UDP_IP, UDP_PORT))

                if not args.headless:
                    y = 50
                    for action in ("SHOOT", "LOOK_LEFT", "LOOK_RIGHT", "WALK_FORWARD"):
                        if action in active:
                            cv2.putText(
                                frame, action, (20, y),
                                cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 0, 255), 3,
                            )
                            y += 40

                if not args.headless:
                    cv2.imshow("gesture_shoot -- press q to quit", frame)
                    if cv2.waitKey(1) & 0xFF == ord("q"):
                        break
                # Headless mode has no window/waitKey to check for 'q' --
                # Godot kills this process directly (OS.kill) when you
                # press G again in-game. KeyboardInterrupt below covers
                # Ctrl+C if you're running it by hand with --headless.
    except KeyboardInterrupt:
        pass
    finally:
        cap.release()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
