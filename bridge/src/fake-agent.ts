import { randomUUID } from "node:crypto";

import { EventStore } from "./event-store.js";
import {
  AUDIO_BYTES_PER_SECOND,
  type SessionState,
  type SessionSummary,
  ProtocolError,
} from "./protocol.js";

export const FAKE_SESSION_ID = "ses_fake_local";

type ApprovalChoice = "approve_once" | "decline" | "cancel";

interface ActiveTurn {
  turnId: string;
  captureId: string;
  prompt: string;
  pieces: string[];
  nextPiece: number;
  fullText: string;
  approvalAfterPiece: number | null;
  timer: NodeJS.Timeout | null;
}

export class FakeAgent {
  #state: SessionState = "idle";
  #activeTurn: ActiveTurn | null = null;
  #pendingApprovalId: string | null = null;

  public constructor(
    private readonly events: EventStore,
    private readonly deltaIntervalMs = 80,
  ) {
    this.events.append(FAKE_SESSION_ID, "connection.ready", {
      bridge: "3gent-0.6-hwtest",
      adapter: "fake",
    });
    this.#emitSessionUpdated();
  }

  public session(): SessionSummary {
    return {
      sessionId: FAKE_SESSION_ID,
      label: "Local fake agent",
      adapter: "fake",
      state: this.#state,
      activeTurnId: this.#activeTurn?.turnId ?? null,
      pendingApprovalId: this.#pendingApprovalId,
      lastSequence: this.events.latestSequence(FAKE_SESSION_ID),
    };
  }

  public submitText(text: string): void {
    this.#ensureIdle();
    const captureId = `cap_${randomUUID()}`;
    this.events.append(FAKE_SESSION_ID, "capture.accepted", {
      captureId,
      kind: "text",
      bytes: Buffer.byteLength(text, "utf8"),
    });
    this.#startTurn(captureId, text);
  }

  public submitAudio(byteLength: number): void {
    this.#ensureIdle();
    const captureId = `cap_${randomUUID()}`;
    const durationMs = Math.floor(byteLength * 1000 / AUDIO_BYTES_PER_SECOND);
    this.events.append(FAKE_SESSION_ID, "capture.accepted", {
      captureId,
      kind: "audio",
      bytes: byteLength,
      durationMs,
      format: "pcm_s16le",
      sampleRate: 16_364,
      channels: 1,
    });
    this.#startTurn(
      captureId,
      `Voice capture received (${durationMs} ms; transcription is mocked).`,
    );
  }

  public interrupt(): void {
    const turn = this.#activeTurn;
    if (turn === null) {
      throw new ProtocolError(409, "NO_ACTIVE_TURN", "no turn is active");
    }
    this.#clearTimer(turn);
    if (this.#pendingApprovalId !== null) {
      this.events.append(FAKE_SESSION_ID, "approval.resolved", {
        approvalId: this.#pendingApprovalId,
        choice: "cancel",
        reason: "turn_interrupted",
      });
      this.#pendingApprovalId = null;
    }
    this.events.append(FAKE_SESSION_ID, "turn.interrupted", {
      turnId: turn.turnId,
    });
    this.events.append(FAKE_SESSION_ID, "turn.completed", {
      turnId: turn.turnId,
      outcome: "interrupted",
    });
    this.#activeTurn = null;
    this.#setState("idle");
  }

  public respondToApproval(
    approvalId: string,
    choice: ApprovalChoice,
  ): void {
    const turn = this.#activeTurn;
    if (turn === null || this.#pendingApprovalId !== approvalId) {
      throw new ProtocolError(
        409,
        "APPROVAL_NOT_PENDING",
        "approval is no longer pending",
      );
    }
    this.events.append(FAKE_SESSION_ID, "approval.resolved", {
      approvalId,
      choice,
    });
    this.#pendingApprovalId = null;

    const resolution = choice === "approve_once"
      ? "Approval accepted. The fake command completed safely.\n"
      : "Approval declined. The fake command was not run.\n";
    turn.pieces.push(resolution);
    this.#setState("working");
    this.#scheduleNextDelta(turn);
  }

  public shutdown(): void {
    if (this.#activeTurn !== null) {
      this.#clearTimer(this.#activeTurn);
    }
  }

  #startTurn(captureId: string, prompt: string): void {
    const turnId = `turn_${randomUUID()}`;
    const preview = prompt.length > 120 ? `${prompt.slice(0, 120)}…` : prompt;
    const pieces = [
      `Fake agent received: ${preview}\n`,
      "This response is emitted as structured 3gent text-delta events.\n",
      "Stage 1 keeps agent-specific details behind the adapter boundary.\n",
    ];
    const needsApproval = /\b(?:approve|approval)\b/i.test(prompt);
    this.#activeTurn = {
      turnId,
      captureId,
      prompt,
      pieces,
      nextPiece: 0,
      fullText: "",
      approvalAfterPiece: needsApproval ? 1 : null,
      timer: null,
    };

    this.events.append(FAKE_SESSION_ID, "turn.started", {
      turnId,
      captureId,
    });
    this.#setState("working");
    this.#scheduleNextDelta(this.#activeTurn);
  }

  #scheduleNextDelta(turn: ActiveTurn): void {
    this.#clearTimer(turn);
    turn.timer = setTimeout(() => this.#emitNextDelta(turn), this.deltaIntervalMs);
  }

  #emitNextDelta(turn: ActiveTurn): void {
    if (this.#activeTurn !== turn || this.#state !== "working") {
      return;
    }

    const piece = turn.pieces[turn.nextPiece];
    if (piece === undefined) {
      this.#completeTurn(turn);
      return;
    }

    turn.nextPiece += 1;
    turn.fullText += piece;
    this.events.append(FAKE_SESSION_ID, "assistant.text.delta", {
      turnId: turn.turnId,
      text: piece,
    });

    if (turn.approvalAfterPiece === turn.nextPiece) {
      this.#requestApproval(turn);
      return;
    }
    this.#scheduleNextDelta(turn);
  }

  #requestApproval(turn: ActiveTurn): void {
    const approvalId = `apr_${randomUUID()}`;
    this.#pendingApprovalId = approvalId;
    this.events.append(FAKE_SESSION_ID, "approval.requested", {
      approvalId,
      turnId: turn.turnId,
      kind: "command",
      summary: "Run the fake Stage 1 test command",
      details: {
        command: "npm test",
        cwd: "/fake/project",
      },
      choices: ["approve_once", "decline", "cancel"],
      expiresAt: new Date(Date.now() + 5 * 60_000).toISOString(),
    });
    this.#setState("waiting_for_user");
  }

  #completeTurn(turn: ActiveTurn): void {
    this.#clearTimer(turn);
    this.events.append(FAKE_SESSION_ID, "assistant.text.completed", {
      turnId: turn.turnId,
      text: turn.fullText,
    });
    this.events.append(FAKE_SESSION_ID, "turn.completed", {
      turnId: turn.turnId,
      outcome: "completed",
    });
    this.#activeTurn = null;
    this.#setState("idle");
  }

  #setState(state: SessionState): void {
    this.#state = state;
    this.#emitSessionUpdated();
  }

  #emitSessionUpdated(): void {
    this.events.append(FAKE_SESSION_ID, "session.updated", {
      state: this.#state,
      activeTurnId: this.#activeTurn?.turnId ?? null,
      pendingApprovalId: this.#pendingApprovalId,
    });
  }

  #clearTimer(turn: ActiveTurn): void {
    if (turn.timer !== null) {
      clearTimeout(turn.timer);
      turn.timer = null;
    }
  }

  #ensureIdle(): void {
    if (this.#activeTurn !== null) {
      throw new ProtocolError(
        409,
        "SESSION_BUSY",
        "session already has an active turn",
        true,
      );
    }
  }
}
