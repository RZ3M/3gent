# 3gent — UI/UX Direction

**Status:** Draft

## 1. Device philosophy

The 3DS should feel like a handheld control surface, not a squeezed desktop UI.

Use physical controls aggressively.

## 2. Screen roles

### Top screen

Primary read surface:
- active repo/session name;
- agent state;
- response text;
- concise diff/approval summary;
- errors.

### Bottom screen

Primary action surface:
- input mode;
- push-to-talk state;
- session switcher;
- approval buttons;
- connection state;
- progress.

## 3. Default interaction model

### Main session view

Top:
```text
3gent
repo-name · Codex
WORKING

> Fix the parser bug...

I found the issue in ...
...
```

Bottom:
```text
[ Hold to Talk ]

[Type] [Capture] [Sessions]

Connection: Remote
```

## 4. Physical-button proposal

Not locked; test ergonomics.

Candidate:
- `R` hold: push-to-talk;
- `A`: primary/select;
- `B`: back/cancel;
- `X`: type prompt;
- `Y`: sessions/status;
- `Start`: interrupt/stop menu;
- D-pad/Circle Pad: scroll/navigation.

Do not make destructive approvals a reflexive single-button action.

## 5. Voice UX

Default:
1. Hold push-to-talk.
2. Visible recording timer + level/activity indicator.
3. Release.
4. Send automatically OR review depending on product decision.
5. Upload progress.
6. Transcription state.
7. Agent working state.

Pending decision:
- auto-send after release;
- or show transcript/review first.

## 6. Text UX

Use the native software keyboard for MVP.

Avoid implementing a custom keyboard unless a real limitation forces it.

## 7. Approval UX

Show:
- what wants permission;
- what it will affect;
- command/path where relevant;
- whether approval is once/session/persistent, if supported and allowed.

Safer interaction example:

```text
Top:
APPROVAL REQUIRED

Run:
npm test

Directory:
/project

Bottom:
[A] Details
[X] Approve once
[B] Decline
[START] Stop agent
```

Exact buttons are pending usability testing.

## 8. Camera and QR

Pairing:
- desktop shows QR;
- user chooses Pair on 3DS;
- camera view opens;
- QR scan gives immediate success/failure;
- manual fallback remains available.

Future photo capture:
- shoot;
- preview;
- optional prompt;
- send.

## 9. Stylus capture

Future sketch mode:
- full bottom-screen canvas;
- clear;
- undo;
- pen thickness;
- send.

The top screen can show the prompt/context while drawing.

## 10. Accessibility/reliability principles

- every network action has visible status;
- every failure has a retry path;
- QR has manual fallback;
- voice has text fallback;
- do not rely only on color to show agent state;
- avoid tiny interaction targets.
