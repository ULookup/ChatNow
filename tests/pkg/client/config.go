package client

import (
	"os"

	"gopkg.in/yaml.v3"
)

type Config struct {
	Target   TargetConfig   `yaml:"target"`
	Timeout  TimeoutConfig  `yaml:"timeout"`
	Database DatabaseConfig `yaml:"database"`
	Log      LogConfig      `yaml:"log"`
}

type TargetConfig struct {
	GatewayAddr   string `yaml:"gateway_addr"`
	WebsocketAddr string `yaml:"websocket_addr"`
}

type TimeoutConfig struct {
	HTTPRequestSec int `yaml:"http_request_sec"`
	WSReadSec      int `yaml:"ws_read_sec"`
}

type DatabaseConfig struct {
	MySQLDSN   string   `yaml:"mysql_dsn"`
	ESURL      string   `yaml:"es_url"`
	RedisNodes []string `yaml:"redis_nodes"`
}

type LogConfig struct {
	Level string `yaml:"level"`
}

func LoadConfig(path string) *Config {
	if path == "" {
		path = "config.yaml"
	}
	data, err := os.ReadFile(path)
	if err != nil {
		panic("failed to read config: " + err.Error())
	}
	cfg := &Config{}
	if err := yaml.Unmarshal(data, cfg); err != nil {
		panic("failed to parse config: " + err.Error())
	}
	// Env overrides for CI
	if v := os.Getenv("GATEWAY_ADDR"); v != "" {
		cfg.Target.GatewayAddr = v
	}
	if v := os.Getenv("WEBSOCKET_ADDR"); v != "" {
		cfg.Target.WebsocketAddr = v
	}
	if v := os.Getenv("MYSQL_DSN"); v != "" {
		cfg.Database.MySQLDSN = v
	}
	if v := os.Getenv("ES_URL"); v != "" {
		cfg.Database.ESURL = v
	}
	return cfg
}
