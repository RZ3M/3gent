import type { SessionSummary } from "./protocol.js";

export type ApprovalChoice = "approve_once" | "decline" | "cancel";

export interface StartSessionOptions {
  cwd: string;
}

export interface CaptureAttachment {
  kind: "photo";
  path: string;
}

export interface AgentAdapter {
  readonly id: string;

  listSessions(): readonly SessionSummary[];
  session(sessionId: string): SessionSummary | undefined;

  startSession(options: StartSessionOptions): Promise<SessionSummary>;
  resumeSession(sessionId: string): Promise<SessionSummary>;
  submitText(
    sessionId: string,
    text: string,
    attachments?: readonly CaptureAttachment[],
  ): Promise<void>;
  interrupt(sessionId: string): Promise<void>;
  respondToApproval(
    sessionId: string,
    approvalId: string,
    choice: ApprovalChoice,
  ): Promise<void>;

  submitAudio?(sessionId: string, byteLength: number): Promise<void>;
  shutdown(): Promise<void>;
}
