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
	Infra    InfraConfig    `yaml:"infra"`
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

type InfraConfig struct {
	RedisContainer string `yaml:"redis_container"`
	PushContainer  string `yaml:"push_container"`
	PushVars       string `yaml:"push_vars"`
	ComposeDir     string `yaml:"compose_dir"`
	TransmiteVars  string `yaml:"transmite_vars"`
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
	if v := os.Getenv("REDIS_CONTAINER"); v != "" {
		cfg.Infra.RedisContainer = v
	}
	if v := os.Getenv("PUSH_CONTAINER"); v != "" {
		cfg.Infra.PushContainer = v
	}
	if v := os.Getenv("PUSH_VARS"); v != "" {
		cfg.Infra.PushVars = v
	}
	if v := os.Getenv("COMPOSE_DIR"); v != "" {
		cfg.Infra.ComposeDir = v
	}
	if v := os.Getenv("TRANSMITE_VARS"); v != "" {
		cfg.Infra.TransmiteVars = v
	}
	return cfg
}
