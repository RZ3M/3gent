# Use after the keyboard + LAN round trip works

Read `AGENTS.md`, `docs/RESEARCH.md`, and the current implementation first.

Stage 0 keyboard + LAN messaging has now been tested on real hardware.

Your next task is to implement the smallest **push-to-talk microphone feasibility spike**.

Goals:
- choose the current libctru microphone API based on official/current sources;
- hold a physical button to record;
- use a bounded buffer or bounded temporary file;
- show recording state and duration;
- stop cleanly on release;
- report format/sample-rate details;
- save or upload the captured bytes to the Stage 0 development server;
- do NOT add transcription yet unless a tiny mock endpoint makes validation materially easier;
- do NOT build the production relay;
- do NOT add camera/sketch features.

Measure and record:
- hardware model;
- sample format/rate;
- memory usage estimate;
- practical duration;
- upload size;
- audible/inspectable validation method;
- failure behavior.

Update `docs/RESEARCH.md` with measured results.

If microphone API details differ from the current docs, follow the current official devkitPro/libctru sources and document the difference.

Stop after the microphone capture/upload feasibility is proven or an honest blocker is identified.
