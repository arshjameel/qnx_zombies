"""
Usage:
    pip install opencv-python mediapipe
    python gesture_shoot.py
    (press 'q' in the preview window to quit)
"""

import argparse
import math
import socket

import cv2
import mediapipe as mp

UDP_IP = "127.0.0.1"
UDP_PORT = 5555

HANDEDNESS_FOR_LEFT = "Left"
HANDEDNESS_FOR_RIGHT = "Right"
HANDEDNESS_MIN_CONFIDENCE = 0.6

WRIST = 0
THUMB_TIP, THUMB_IP, THUMB_MCP = 4, 3, 2
INDEX_TIP, INDEX_PIP, INDEX_MCP = 8, 6, 5
MIDDLE_TIP, MIDDLE_PIP, MIDDLE_MCP = 12, 10, 9
RING_TIP, RING_PIP = 16, 14
PINKY_TIP, PINKY_PIP = 20, 18

EXTEND_RATIO = 1.1

THUMB_SPREAD_RATIO = 0.5

RAISED_Y_THRESHOLD = 0.5


def dist(a, b) -> float:
    return math.sqrt((a.x - b.x) ** 2 + (a.y - b.y) ** 2 + (a.z - b.z) ** 2)


def is_extended(lm, tip, pip) -> bool:
    return dist(lm[WRIST], lm[tip]) > dist(lm[WRIST], lm[pip]) * EXTEND_RATIO


def is_raised(lm) -> bool:
    return lm[WRIST].y < RAISED_Y_THRESHOLD


def is_fist(lm) -> bool:
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
    except KeyboardInterrupt:
        pass
    finally:
        cap.release()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
