/*
 * Presentation for a `QrMatrix`. A terminal rendering is the fast path while
 * the user is already at the shell; the SVG exists because a browser at full
 * screen brightness is a far easier scanning target for a 2011 camera than a
 * terminal font is.
 */
import type { QrMatrix } from "./qr-encode.js";

const QUIET_MODULES = 4;

/**
 * Renders the matrix with half-block characters, so one text row carries two
 * module rows and the code stays close to square in a normal terminal cell
 * grid. Dark modules are drawn light-on-dark because scanners expect dark
 * modules on a light background, and a terminal's default background is not
 * something we can rely on.
 */
export function renderQrToTerminal(matrix: QrMatrix): string {
  const span = matrix.size + 2 * QUIET_MODULES;
  const isDark = (row: number, column: number): boolean => {
    const y = row - QUIET_MODULES;
    const x = column - QUIET_MODULES;
    if (y < 0 || x < 0 || y >= matrix.size || x >= matrix.size) {
      return false;
    }
    return matrix.modules[y]?.[x] === true;
  };

  const lines: string[] = [];
  for (let row = 0; row < span; row += 2) {
    let line = "";
    for (let column = 0; column < span; column += 1) {
      const upper = isDark(row, column);
      const lower = row + 1 < span && isDark(row + 1, column);
      if (upper && lower) {
        line += " ";
      } else if (upper) {
        line += "▄";
      } else if (lower) {
        line += "▀";
      } else {
        line += "█";
      }
    }
    lines.push(line);
  }
  return lines.join("\n");
}

/** Renders the matrix as a standalone SVG document. */
export function renderQrToSvg(matrix: QrMatrix, moduleSize = 8): string {
  const span = matrix.size + 2 * QUIET_MODULES;
  const side = span * moduleSize;
  const rectangles: string[] = [];
  for (let row = 0; row < matrix.size; row += 1) {
    for (let column = 0; column < matrix.size; column += 1) {
      if (matrix.modules[row]?.[column] !== true) {
        continue;
      }
      const x = (column + QUIET_MODULES) * moduleSize;
      const y = (row + QUIET_MODULES) * moduleSize;
      rectangles.push(
        `<rect x="${x}" y="${y}" width="${moduleSize}" height="${moduleSize}"/>`,
      );
    }
  }
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${side}" height="${side}"`
    + ` viewBox="0 0 ${side} ${side}" shape-rendering="crispEdges">\n`
    + `<rect width="${side}" height="${side}" fill="#FFFFFF"/>\n`
    + `<g fill="#000000">${rectangles.join("")}</g>\n`
    + "</svg>\n";
}
