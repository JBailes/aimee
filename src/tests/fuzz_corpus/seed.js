import { readFile } from 'fs';
import path from 'path';
const axios = require('axios');

export function parseConfig(data) {
  return JSON.parse(data);
}

export class ConfigManager {
  constructor(path) {
    this.path = path;
  }
  load() { return readFile(this.path); }
}

const DEFAULT_TIMEOUT = 5000;
module.exports = { parseConfig, DEFAULT_TIMEOUT };
