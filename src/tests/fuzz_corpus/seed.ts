import { Request, Response } from 'express';
import * as crypto from 'crypto';

export interface Config {
  host: string;
  port: number;
}

export type Handler = (req: Request, res: Response) => void;

export function createServer(config: Config): void {
  console.log(`Starting on ${config.host}:${config.port}`);
}

export class Router {
  private routes: Map<string, Handler> = new Map();
  add(path: string, handler: Handler) { this.routes.set(path, handler); }
}

export enum Status { OK, ERROR, PENDING }
