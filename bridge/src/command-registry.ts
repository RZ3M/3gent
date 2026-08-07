import type { CommandAcknowledgement } from "./protocol.js";

export class CommandRegistry {
  readonly #commands = new Map<string, CommandAcknowledgement>();

  public constructor(private readonly capacity = 256) {
    if (capacity <= 0) {
      throw new RangeError("capacity must be positive");
    }
  }

  public get(commandId: string): CommandAcknowledgement | undefined {
    const acknowledgement = this.#commands.get(commandId);
    if (acknowledgement === undefined) {
      return undefined;
    }
    return {...acknowledgement, duplicate: true};
  }

  public remember(acknowledgement: CommandAcknowledgement): void {
    this.#commands.set(acknowledgement.commandId, acknowledgement);
    while (this.#commands.size > this.capacity) {
      const oldest = this.#commands.keys().next().value as string | undefined;
      if (oldest === undefined) {
        return;
      }
      this.#commands.delete(oldest);
    }
  }
}
