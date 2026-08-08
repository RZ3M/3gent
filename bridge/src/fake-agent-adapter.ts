import type {
  AgentAdapter,
  ApprovalChoice,
  CaptureAttachment,
  StartSessionOptions,
} from "./agent-adapter.js";
import { EventStore } from "./event-store.js";
import { FAKE_SESSION_ID, FakeAgent } from "./fake-agent.js";
import type { SessionSummary } from "./protocol.js";

export class FakeAgentAdapter implements AgentAdapter {
  public readonly id = "fake";
  readonly #agent: FakeAgent;

  public constructor(events: EventStore, deltaIntervalMs = 80) {
    this.#agent = new FakeAgent(events, deltaIntervalMs);
  }

  public listSessions(): readonly SessionSummary[] {
    return [this.#agent.session()];
  }

  public session(sessionId: string): SessionSummary | undefined {
    return sessionId === FAKE_SESSION_ID ? this.#agent.session() : undefined;
  }

  public async startSession(
    _options: StartSessionOptions,
  ): Promise<SessionSummary> {
    return this.#agent.session();
  }

  public async resumeSession(sessionId: string): Promise<SessionSummary> {
    const session = this.session(sessionId);
    if (session === undefined) {
      throw new Error("fake session does not exist");
    }
    return session;
  }

  public async submitText(
    _sessionId: string,
    text: string,
    attachments: readonly CaptureAttachment[] = [],
  ): Promise<void> {
    this.#agent.submitText(
      attachments.length > 0 ? `${text}\n[${attachments.length} photo attached]` : text,
    );
  }

  public async submitAudio(
    _sessionId: string,
    byteLength: number,
  ): Promise<void> {
    this.#agent.submitAudio(byteLength);
  }

  public async interrupt(_sessionId: string): Promise<void> {
    this.#agent.interrupt();
  }

  public async respondToApproval(
    _sessionId: string,
    approvalId: string,
    choice: ApprovalChoice,
  ): Promise<void> {
    this.#agent.respondToApproval(approvalId, choice);
  }

  public async shutdown(): Promise<void> {
    this.#agent.shutdown();
  }
}
