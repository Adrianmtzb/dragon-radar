#!/usr/bin/env node
// MCP server (stdio) that wraps the Dragon Radar HTTP API (docs/api.md) as tools.
// Devices: DRAGONRADAR_HOSTS="dragonradar.local,192.168.1.50" (hostnames or IPs; the first is the default).
import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { fileURLToPath } from 'node:url';
import { z } from 'zod';
import { COMPASS, MAX_BALLS, RADAR_KEYS, SFX, SWEEP_MODES, THEMES, ZOOM_LABELS, getState, playSfx, setBalls, setRadar } from './tools.mjs';

export const HOSTS = (process.env.DRAGONRADAR_HOSTS || 'dragonradar.local')
  .split(',')
  .map((h) => h.trim())
  .filter(Boolean);

function resolveDevices(device, hosts) {
  if (!device) return [hosts[0]];
  if (device === 'all') return hosts;
  const d = device.toLowerCase();
  const match = hosts.find((h) => h.toLowerCase() === d || h.toLowerCase().startsWith(d + '.') || h.toLowerCase().includes(d));
  return [match || device];
}

// Runs fn against every target device and returns one text result.
async function onDevices(device, hosts, fn) {
  const lines = await Promise.all(
    resolveDevices(device, hosts).map(async (h) => {
      try {
        return { ok: true, text: `${h}: ${await fn(h)}` };
      } catch (e) {
        return { ok: false, text: `${h}: error - ${e.message}` };
      }
    }),
  );
  return { content: [{ type: 'text', text: lines.map((l) => l.text).join('\n') }], isError: lines.every((l) => !l.ok) };
}

export function createServer(hosts = HOSTS) {
  const server = new McpServer({ name: 'dragon-radar', version: '0.1.0' });
  const deviceArg = z
    .string()
    .optional()
    .describe(`Target radar: hostname, IP, part of a name, or "all". Defaults to ${hosts[0]}. Known: ${hosts.join(', ')}`);
  const bearingArg = z
    .union([z.number(), z.string()])
    .describe(`Direction of the ball from the user: degrees clockwise from the radar's north (0-359.9) or a compass point (${COMPASS.join(', ')})`);
  const distanceArg = z.union([z.number(), z.string()]).describe('Distance: metres (300) or a string with units ("300 m", "1.5 km"); 0.1 m to 100 km');

  server.registerTool(
    'get_state',
    {
      title: 'Get radar state',
      description: 'What the Dragon Radar shows now: power, zoom level, theme, sweep, volume, heading, the balls (stars, bearing, distance), the nearest ball and the beep interval.',
      inputSchema: { device: deviceArg },
    },
    async ({ device }) => onDevices(device, hosts, (h) => getState(h)),
  );

  server.registerTool(
    'set_radar',
    {
      title: 'Set radar',
      description: `Change any of ${RADAR_KEYS.join(', ')}. Zoom is the scale of one grid cell. power "on" from off plays the boot animation; resetHeading makes the current orientation the radar's north.`,
      inputSchema: {
        zoom: z.union([z.number().int().min(0).max(ZOOM_LABELS.length - 1), z.enum(ZOOM_LABELS)]).optional().describe(`Level 0-${ZOOM_LABELS.length - 1} or its cell size: ${ZOOM_LABELS.join(', ')}`),
        theme: z.enum(THEMES).optional().describe('z = Dragon Ball Z (Namek), classic = Dragon Ball 1986, ptp = The Path to Power'),
        sweep: z.enum(SWEEP_MODES).optional().describe('Scan effect: line = a turning hand with a trail, ring = a ring that grows on every beep, both, or none'),
        volume: z.number().int().min(0).max(100).optional(),
        muted: z.boolean().optional(),
        power: z.enum(['on', 'off']).optional(),
        resetHeading: z.boolean().optional(),
        device: deviceArg,
      },
    },
    async ({ device, ...args }) => onDevices(device, hosts, (h) => setRadar(h, args)),
  );

  server.registerTool(
    'set_balls',
    {
      title: 'Place dragon balls',
      description:
        `Put up to ${MAX_BALLS} dragon balls on the radar by bearing and distance from the user (the radar then shows them and beeps faster as the nearest gets closer). ` +
        'Use keep to move or add balls without touching the others; demo restores the 7 demo balls; clear leaves none.',
      inputSchema: {
        balls: z
          .array(z.object({ stars: z.number().int().min(1).max(7).describe('Which ball: 1-7 stars'), bearing: bearingArg, distance: distanceArg }))
          .max(MAX_BALLS)
          .optional(),
        keep: z.boolean().optional().describe('Merge with the balls on the radar now (a ball with the same stars is moved)'),
        demo: z.boolean().optional().describe('Go back to the 7 demo balls'),
        clear: z.boolean().optional().describe('Remove every ball'),
        device: deviceArg,
      },
    },
    async ({ device, ...args }) => onDevices(device, hosts, (h) => setBalls(h, args)),
  );

  server.registerTool(
    'play_sfx',
    {
      title: 'Play radar sound',
      description: 'Play one of the radar sounds on the board speaker (silent while the radar is off or muted). button is the "brrr" every button or screen press makes; radar_beep is the "pi-PI".',
      inputSchema: { name: z.enum(SFX), device: deviceArg },
    },
    async ({ name, device }) => onDevices(device, hosts, (h) => playSfx(h, name)),
  );

  return server;
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  await createServer().connect(new StdioServerTransport());
}
