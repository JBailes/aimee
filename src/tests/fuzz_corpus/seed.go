package main

import (
	"fmt"
	"net/http"
	"os"
)

type Config struct {
	Host string
	Port int
}

type Server struct {
	config Config
}

func NewServer(cfg Config) *Server {
	return &Server{config: cfg}
}

func (s *Server) Start() error {
	addr := fmt.Sprintf("%s:%d", s.config.Host, s.config.Port)
	return http.ListenAndServe(addr, nil)
}

func loadEnv(key string) string {
	return os.Getenv(key)
}
