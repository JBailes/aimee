import 'dart:io';
import 'package:flutter/material.dart';

class Config {
  final String host;
  final int port;
  Config({required this.host, required this.port});
}

enum AppState { loading, ready, error }

mixin Logging {
  void log(String msg) => print(msg);
}

class Server with Logging {
  final Config config;
  Server(this.config);

  Future<void> start() async {
    log('Starting on ${config.host}:${config.port}');
  }
}

void main() {
  final server = Server(Config(host: 'localhost', port: 8080));
  server.start();
}
