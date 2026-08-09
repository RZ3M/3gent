/*
 * A byte-mode QR encoder, limited to what pairing actually needs: ISO/IEC 18004
 * versions 1-10 at error-correction level M.
 *
 * It is written out rather than taken from a dependency because the bridge has
 * no runtime dependencies today, and because the only consumer is a fixed,
 * short ASCII pairing URL. Level M is chosen over L so a handheld camera has
 * error-correction headroom, and the version ceiling keeps the module grid
 * coarse enough for a 400x240 sensor.
 *
 * `tools/qr-check/` decodes this encoder's output with the same vendored quirc
 * build that runs on the 3DS, which is the check that matters.
 */

export const QR_MIN_VERSION = 1;
export const QR_MAX_VERSION = 10;

interface VersionSpec {
  /** Error-correction codewords per block. */
  eccPerBlock: number;
  /** [blockCount, dataCodewordsPerBlock] for the first (shorter) group. */
  group1: readonly [number, number];
  /** [blockCount, dataCodewordsPerBlock] for the second group, if any. */
  group2: readonly [number, number] | null;
  /** Row/column centres of the alignment patterns. */
  alignment: readonly number[];
}

/* ISO/IEC 18004 tables 9 and E.1, level M only. */
const VERSIONS: readonly VersionSpec[] = [
  {eccPerBlock: 10, group1: [1, 16], group2: null, alignment: []},
  {eccPerBlock: 16, group1: [1, 28], group2: null, alignment: [6, 18]},
  {eccPerBlock: 26, group1: [1, 44], group2: null, alignment: [6, 22]},
  {eccPerBlock: 18, group1: [2, 32], group2: null, alignment: [6, 26]},
  {eccPerBlock: 24, group1: [2, 43], group2: null, alignment: [6, 30]},
  {eccPerBlock: 16, group1: [4, 27], group2: null, alignment: [6, 34]},
  {eccPerBlock: 18, group1: [4, 31], group2: null, alignment: [6, 22, 38]},
  {eccPerBlock: 22, group1: [2, 38], group2: [2, 39], alignment: [6, 24, 42]},
  {eccPerBlock: 22, group1: [3, 36], group2: [2, 37], alignment: [6, 26, 46]},
  {eccPerBlock: 26, group1: [4, 43], group2: [1, 44], alignment: [6, 28, 50]},
];

/* Remainder bits appended after the interleaved codeword stream. */
const REMAINDER_BITS: readonly number[] = [0, 7, 7, 7, 7, 7, 0, 0, 0, 0];

const FORMAT_MASK = 0b101010000010010;
const FORMAT_GENERATOR = 0b10100110111;
const VERSION_GENERATOR = 0b1111100100101;
/* Level M is 0b00 in the two-bit error-correction indicator. */
const ECC_INDICATOR_M = 0b00;

export interface QrMatrix {
  version: number;
  size: number;
  /** Row-major module grid; true is a dark module. */
  modules: readonly (readonly boolean[])[];
}

/* ------------------------------------------------------------ GF(256) --- */

const GF_EXP = new Uint8Array(512);
const GF_LOG = new Uint8Array(256);

{
  let value = 1;
  for (let index = 0; index < 255; index += 1) {
    GF_EXP[index] = value;
    GF_LOG[value] = index;
    value <<= 1;
    if ((value & 0x100) !== 0) {
      value ^= 0x11d;
    }
  }
  for (let index = 255; index < 512; index += 1) {
    GF_EXP[index] = GF_EXP[index - 255] ?? 0;
  }
}

function gfMultiply(left: number, right: number): number {
  if (left === 0 || right === 0) {
    return 0;
  }
  return GF_EXP[(GF_LOG[left] ?? 0) + (GF_LOG[right] ?? 0)] ?? 0;
}

function reedSolomonGenerator(degree: number): Uint8Array {
  let polynomial = Uint8Array.from([1]);
  for (let index = 0; index < degree; index += 1) {
    const next = new Uint8Array(polynomial.length + 1);
    for (let position = 0; position < polynomial.length; position += 1) {
      const coefficient = polynomial[position] ?? 0;
      next[position] = (next[position] ?? 0) ^ coefficient;
      next[position + 1] = (next[position + 1] ?? 0)
        ^ gfMultiply(coefficient, GF_EXP[index] ?? 0);
    }
    polynomial = next;
  }
  return polynomial;
}

function reedSolomonRemainder(data: Uint8Array, degree: number): Uint8Array {
  const generator = reedSolomonGenerator(degree);
  const remainder = new Uint8Array(degree);
  for (const byte of data) {
    const factor = byte ^ (remainder[0] ?? 0);
    remainder.copyWithin(0, 1);
    remainder[degree - 1] = 0;
    for (let index = 0; index < degree; index += 1) {
      remainder[index] = (remainder[index] ?? 0)
        ^ gfMultiply(generator[index + 1] ?? 0, factor);
    }
  }
  return remainder;
}

/* -------------------------------------------------------------- bits --- */

class BitWriter {
  readonly #bits: number[] = [];

  public write(value: number, length: number): void {
    for (let index = length - 1; index >= 0; index -= 1) {
      this.#bits.push((value >>> index) & 1);
    }
  }

  public get length(): number {
    return this.#bits.length;
  }

  public toCodewords(count: number): Uint8Array {
    const codewords = new Uint8Array(count);
    for (let index = 0; index < this.#bits.length; index += 1) {
      if (this.#bits[index] === 1) {
        const byte = index >>> 3;
        codewords[byte] = (codewords[byte] ?? 0) | (0x80 >>> (index & 7));
      }
    }
    return codewords;
  }
}

function specFor(version: number): VersionSpec {
  const spec = VERSIONS[version - 1];
  if (spec === undefined) {
    throw new Error(`unsupported QR version ${version}`);
  }
  return spec;
}

function dataCodewordCount(spec: VersionSpec): number {
  const [blocks1, size1] = spec.group1;
  const group2 = spec.group2;
  return blocks1 * size1 + (group2 === null ? 0 : group2[0] * group2[1]);
}

function characterCountBits(version: number): number {
  return version < 10 ? 8 : 16;
}

/** Largest byte-mode payload the given version holds at level M. */
export function byteCapacity(version: number): number {
  const spec = specFor(version);
  const overheadBits = 4 + characterCountBits(version);
  return Math.floor((dataCodewordCount(spec) * 8 - overheadBits) / 8);
}

function chooseVersion(byteLength: number): number {
  for (let version = QR_MIN_VERSION; version <= QR_MAX_VERSION; version += 1) {
    if (byteLength <= byteCapacity(version)) {
      return version;
    }
  }
  throw new Error(
    `payload of ${byteLength} bytes exceeds the version ${QR_MAX_VERSION} `
    + `capacity of ${byteCapacity(QR_MAX_VERSION)} bytes`,
  );
}

/* ------------------------------------------------------------ payload --- */

function buildCodewords(payload: Uint8Array, version: number): Uint8Array {
  const spec = specFor(version);
  const totalDataCodewords = dataCodewordCount(spec);
  const capacityBits = totalDataCodewords * 8;

  const writer = new BitWriter();
  writer.write(0b0100, 4);
  writer.write(payload.length, characterCountBits(version));
  for (const byte of payload) {
    writer.write(byte, 8);
  }
  writer.write(0, Math.min(4, capacityBits - writer.length));
  if (writer.length % 8 !== 0) {
    writer.write(0, 8 - (writer.length % 8));
  }
  const padBytes = (capacityBits - writer.length) / 8;
  for (let index = 0; index < padBytes; index += 1) {
    writer.write(index % 2 === 0 ? 0xec : 0x11, 8);
  }

  const data = writer.toCodewords(totalDataCodewords);

  const dataBlocks: Uint8Array[] = [];
  const eccBlocks: Uint8Array[] = [];
  let offset = 0;
  const groups: readonly (readonly [number, number])[] = spec.group2 === null
    ? [spec.group1]
    : [spec.group1, spec.group2];
  for (const [blockCount, blockSize] of groups) {
    for (let block = 0; block < blockCount; block += 1) {
      const slice = data.subarray(offset, offset + blockSize);
      offset += blockSize;
      dataBlocks.push(slice);
      eccBlocks.push(reedSolomonRemainder(slice, spec.eccPerBlock));
    }
  }

  const interleaved: number[] = [];
  const longestData = Math.max(...dataBlocks.map((block) => block.length));
  for (let index = 0; index < longestData; index += 1) {
    for (const block of dataBlocks) {
      const byte = block[index];
      if (byte !== undefined) {
        interleaved.push(byte);
      }
    }
  }
  for (let index = 0; index < spec.eccPerBlock; index += 1) {
    for (const block of eccBlocks) {
      interleaved.push(block[index] ?? 0);
    }
  }
  return Uint8Array.from(interleaved);
}

/* ------------------------------------------------------------- matrix --- */

type Grid = (boolean | null)[][];

function placeFinder(grid: Grid, reserved: boolean[][], row: number, column: number): void {
  for (let y = -1; y <= 7; y += 1) {
    for (let x = -1; x <= 7; x += 1) {
      const targetY = row + y;
      const targetX = column + x;
      if (targetY < 0 || targetY >= grid.length
        || targetX < 0 || targetX >= grid.length) {
        continue;
      }
      const inRing = (y >= 0 && y <= 6 && (x === 0 || x === 6))
        || (x >= 0 && x <= 6 && (y === 0 || y === 6));
      const inCore = y >= 2 && y <= 4 && x >= 2 && x <= 4;
      (grid[targetY] as (boolean | null)[])[targetX] = inRing || inCore;
      (reserved[targetY] as boolean[])[targetX] = true;
    }
  }
}

function placeAlignment(grid: Grid, reserved: boolean[][], centres: readonly number[]): void {
  const size = grid.length;
  for (const centreY of centres) {
    for (const centreX of centres) {
      const nearFinder = (centreY <= 8 && centreX <= 8)
        || (centreY <= 8 && centreX >= size - 9)
        || (centreY >= size - 9 && centreX <= 8);
      if (nearFinder) {
        continue;
      }
      for (let y = -2; y <= 2; y += 1) {
        for (let x = -2; x <= 2; x += 1) {
          const distance = Math.max(Math.abs(x), Math.abs(y));
          (grid[centreY + y] as (boolean | null)[])[centreX + x] = distance !== 1;
          (reserved[centreY + y] as boolean[])[centreX + x] = true;
        }
      }
    }
  }
}

function reserveFormatAreas(reserved: boolean[][], size: number, version: number): void {
  /* First copy: row 8 and column 8 up to and including their intersection. */
  for (let index = 0; index <= 8; index += 1) {
    (reserved[8] as boolean[])[index] = true;
    (reserved[index] as boolean[])[8] = true;
  }
  /*
   * Second copy: eight modules each. Nine would swallow one data module at
   * each end and silently shift the whole codeword stream.
   */
  for (let index = 0; index < 8; index += 1) {
    (reserved[8] as boolean[])[size - 1 - index] = true;
    (reserved[size - 1 - index] as boolean[])[8] = true;
  }
  if (version >= 7) {
    for (let y = 0; y < 6; y += 1) {
      for (let x = 0; x < 3; x += 1) {
        (reserved[y] as boolean[])[size - 11 + x] = true;
        (reserved[size - 11 + x] as boolean[])[y] = true;
      }
    }
  }
}

function maskBit(mask: number, row: number, column: number): boolean {
  switch (mask) {
    case 0: return (row + column) % 2 === 0;
    case 1: return row % 2 === 0;
    case 2: return column % 3 === 0;
    case 3: return (row + column) % 3 === 0;
    case 4: return (Math.floor(row / 2) + Math.floor(column / 3)) % 2 === 0;
    case 5: return ((row * column) % 2) + ((row * column) % 3) === 0;
    case 6: return ((((row * column) % 2) + ((row * column) % 3)) % 2) === 0;
    default: return ((((row + column) % 2) + ((row * column) % 3)) % 2) === 0;
  }
}

function bchFormatBits(mask: number): number {
  const data = (ECC_INDICATOR_M << 3) | mask;
  let remainder = data << 10;
  for (let bit = 14; bit >= 10; bit -= 1) {
    if (((remainder >>> bit) & 1) !== 0) {
      remainder ^= FORMAT_GENERATOR << (bit - 10);
    }
  }
  return ((data << 10) | remainder) ^ FORMAT_MASK;
}

function bchVersionBits(version: number): number {
  let remainder = version << 12;
  for (let bit = 17; bit >= 12; bit -= 1) {
    if (((remainder >>> bit) & 1) !== 0) {
      remainder ^= VERSION_GENERATOR << (bit - 12);
    }
  }
  return (version << 12) | remainder;
}

function applyFormatInformation(grid: Grid, size: number, mask: number): void {
  const bits = bchFormatBits(mask);
  for (let index = 0; index < 15; index += 1) {
    const bit = ((bits >>> index) & 1) === 1;
    /* Copy one: around the top-left finder, skipping the timing row/column. */
    if (index < 6) {
      (grid[index] as (boolean | null)[])[8] = bit;
    } else if (index === 6) {
      (grid[7] as (boolean | null)[])[8] = bit;
    } else if (index === 7) {
      (grid[8] as (boolean | null)[])[8] = bit;
    } else if (index === 8) {
      (grid[8] as (boolean | null)[])[7] = bit;
    } else {
      (grid[8] as (boolean | null)[])[14 - index] = bit;
    }
    /* Copy two: split between the bottom-left and top-right finders. */
    if (index < 8) {
      (grid[8] as (boolean | null)[])[size - 1 - index] = bit;
    } else {
      (grid[size - 15 + index] as (boolean | null)[])[8] = bit;
    }
  }
  (grid[size - 8] as (boolean | null)[])[8] = true;
}

function applyVersionInformation(grid: Grid, size: number, version: number): void {
  if (version < 7) {
    return;
  }
  const bits = bchVersionBits(version);
  for (let index = 0; index < 18; index += 1) {
    const bit = ((bits >>> index) & 1) === 1;
    const row = Math.floor(index / 3);
    const column = index % 3;
    (grid[row] as (boolean | null)[])[size - 11 + column] = bit;
    (grid[size - 11 + column] as (boolean | null)[])[row] = bit;
  }
}

function penalty(grid: Grid, size: number): number {
  const at = (row: number, column: number): boolean =>
    (grid[row] as (boolean | null)[])[column] === true;
  let score = 0;

  /* Rule 1: runs of five or more identical modules. */
  for (let line = 0; line < size; line += 1) {
    for (const horizontal of [true, false]) {
      let run = 1;
      for (let index = 1; index < size; index += 1) {
        const current = horizontal ? at(line, index) : at(index, line);
        const previous = horizontal ? at(line, index - 1) : at(index - 1, line);
        if (current === previous) {
          run += 1;
        } else {
          if (run >= 5) {
            score += run - 2;
          }
          run = 1;
        }
      }
      if (run >= 5) {
        score += run - 2;
      }
    }
  }

  /* Rule 2: 2x2 blocks of one colour. */
  for (let row = 0; row < size - 1; row += 1) {
    for (let column = 0; column < size - 1; column += 1) {
      const value = at(row, column);
      if (value === at(row, column + 1)
        && value === at(row + 1, column)
        && value === at(row + 1, column + 1)) {
        score += 3;
      }
    }
  }

  /* Rule 3: finder-like 1:1:3:1:1 sequences with four light modules beside. */
  const pattern = [true, false, true, true, true, false, true];
  for (let line = 0; line < size; line += 1) {
    for (let index = 0; index + 7 <= size; index += 1) {
      for (const horizontal of [true, false]) {
        let matches = true;
        for (let offset = 0; offset < 7; offset += 1) {
          const value = horizontal
            ? at(line, index + offset)
            : at(index + offset, line);
          if (value !== pattern[offset]) {
            matches = false;
            break;
          }
        }
        if (!matches) {
          continue;
        }
        const before = [index - 4, index - 3, index - 2, index - 1];
        const after = [index + 7, index + 8, index + 9, index + 10];
        for (const window of [before, after]) {
          if (window.every((position) => position >= 0 && position < size
            && !(horizontal ? at(line, position) : at(position, line)))) {
            score += 40;
          }
        }
      }
    }
  }

  /* Rule 4: deviation from an even light/dark balance. */
  let dark = 0;
  for (let row = 0; row < size; row += 1) {
    for (let column = 0; column < size; column += 1) {
      if (at(row, column)) {
        dark += 1;
      }
    }
  }
  const percent = (dark * 100) / (size * size);
  score += Math.floor(Math.abs(percent - 50) / 5) * 10;
  return score;
}

function drawCodewords(
  grid: Grid,
  reserved: boolean[][],
  size: number,
  codewords: Uint8Array,
  remainderBits: number,
): void {
  const totalBits = codewords.length * 8 + remainderBits;
  let bitIndex = 0;
  let upward = true;
  for (let right = size - 1; right >= 1; right -= 2) {
    /* Column 6 is the vertical timing pattern and is never a data column. */
    const rightColumn = right <= 6 ? right - 1 : right;
    for (let step = 0; step < size; step += 1) {
      const row = upward ? size - 1 - step : step;
      for (const column of [rightColumn, rightColumn - 1]) {
        if ((reserved[row] as boolean[])[column] === true) {
          continue;
        }
        let bit = false;
        if (bitIndex < totalBits) {
          const byte = codewords[bitIndex >>> 3];
          bit = byte !== undefined && ((byte >>> (7 - (bitIndex & 7))) & 1) === 1;
        }
        bitIndex += 1;
        (grid[row] as (boolean | null)[])[column] = bit;
      }
    }
    upward = !upward;
  }
}

/** Encodes ASCII/UTF-8 `text` as a level-M byte-mode QR matrix. */
export function encodeQr(text: string, minimumVersion = QR_MIN_VERSION): QrMatrix {
  const payload = Buffer.from(text, "utf8");
  const version = Math.max(chooseVersion(payload.length), minimumVersion);
  const spec = specFor(version);
  const size = version * 4 + 17;

  const grid: Grid = Array.from(
    {length: size},
    () => Array.from({length: size}, () => null as boolean | null),
  );
  const reserved: boolean[][] = Array.from(
    {length: size},
    () => Array.from({length: size}, () => false),
  );

  placeFinder(grid, reserved, 0, 0);
  placeFinder(grid, reserved, 0, size - 7);
  placeFinder(grid, reserved, size - 7, 0);
  placeAlignment(grid, reserved, spec.alignment);
  for (let index = 8; index < size - 8; index += 1) {
    const dark = index % 2 === 0;
    (grid[6] as (boolean | null)[])[index] = dark;
    (grid[index] as (boolean | null)[])[6] = dark;
    (reserved[6] as boolean[])[index] = true;
    (reserved[index] as boolean[])[6] = true;
  }
  reserveFormatAreas(reserved, size, version);

  const codewords = buildCodewords(payload, version);
  drawCodewords(grid, reserved, size, codewords, REMAINDER_BITS[version - 1] ?? 0);
  applyVersionInformation(grid, size, version);

  let best: {mask: number; modules: boolean[][]} | null = null;
  let bestScore = Number.POSITIVE_INFINITY;
  for (let mask = 0; mask < 8; mask += 1) {
    const candidate: Grid = grid.map((row, rowIndex) => row.map((value, columnIndex) => {
      if ((reserved[rowIndex] as boolean[])[columnIndex] === true) {
        return value;
      }
      return value === true ? !maskBit(mask, rowIndex, columnIndex)
        : maskBit(mask, rowIndex, columnIndex);
    }));
    applyFormatInformation(candidate, size, mask);
    const score = penalty(candidate, size);
    if (score < bestScore) {
      bestScore = score;
      best = {
        mask,
        modules: candidate.map((row) => row.map((value) => value === true)),
      };
    }
  }
  if (best === null) {
    throw new Error("no QR mask could be selected");
  }
  return {version, size, modules: best.modules};
}
