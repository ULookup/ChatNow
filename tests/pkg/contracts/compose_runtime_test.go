package contracts

import (
	"encoding/json"
	"fmt"
	"net"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"testing"

	"github.com/stretchr/testify/require"
	"gopkg.in/yaml.v3"
)

type composeRuntimeDocument struct {
	Services map[string]composeRuntimeService `yaml:"services"`
}

type composeRuntimeService struct {
	Build       any   `yaml:"build"`
	Command     any   `yaml:"command"`
	Entrypoint  any   `yaml:"entrypoint"`
	DependsOn   any   `yaml:"depends_on"`
	Environment any   `yaml:"environment"`
	Healthcheck any   `yaml:"healthcheck"`
	Hostname    string `yaml:"hostname"`
	Ports       []any `yaml:"ports"`
	Restart     any   `yaml:"restart"`
	Volumes     []any `yaml:"volumes"`
}

// Repository contract | P0 | The root Compose topology includes usable object storage.
func TestComposeRuntimeTopologyContract(t *testing.T) {
	root := repositoryRoot(t)
	compose := readRuntimeCompose(t, root)

	t.Run("root stack defines MinIO", func(t *testing.T) {
		minio, exists := compose.Services["minio"]
		require.True(t, exists, "root docker-compose.yml must define minio")
		require.NotNil(t, minio.Healthcheck, "minio must expose a healthcheck for its initializer")
	})

	t.Run("MinIO initializer is bounded and one-shot", func(t *testing.T) {
		initializer, exists := compose.Services["minio-init"]
		require.True(t, exists, "root docker-compose.yml must define minio-init")
		require.Equal(t, "no", strings.ToLower(fmt.Sprint(initializer.Restart)),
			"minio-init must be a one-shot service")
		require.Equal(t, "service_healthy", dependencyCondition(initializer.DependsOn, "minio"),
			"minio-init must wait for MinIO health")
		initializerText := serviceCommandText(initializer)
		require.NotRegexp(t, regexp.MustCompile(`(?i)\btail\s+-f\b`), initializerText,
			"minio-init must exit after bucket initialization")
		require.Contains(t, fmt.Sprint(initializer.Build), "docker/minio-init",
			"the shell-based initializer must use the repository init image, not the shell-less mc image")
		dockerfile, err := os.ReadFile(filepath.Join(root, "docker/minio-init/Dockerfile"))
		require.NoError(t, err)
		require.Regexp(t, regexp.MustCompile(`(?im)^FROM\s+(?:busybox|alpine):[^\s]+\s*$`), string(dockerfile),
			"the final MinIO init stage must provide /bin/sh")
		require.Regexp(t, regexp.MustCompile(`(?im)^COPY\s+--from=mc\s+/usr/bin/mc\s+/usr/bin/mc\s*$`), string(dockerfile),
			"the init image must copy the pinned official mc binary")
		entrypoint, err := os.ReadFile(filepath.Join(root, "docker/minio-init/entrypoint.sh"))
		require.NoError(t, err)
		entrypointSource := string(entrypoint)
		require.Contains(t, entrypointSource, "MC_CONFIG_DIR",
			"temporary mc state must be isolated and removed")
		require.NotRegexp(t,
			regexp.MustCompile(`(?m)^\s*mc\s+alias\s+set[^\n]*(?:MINIO_ROOT_PASSWORD|\$\{?MINIO_ROOT_PASSWORD)`),
			entrypointSource, "the MinIO root password must not enter process arguments")
		require.NotRegexp(t,
			regexp.MustCompile(`(?m)^\s*mc\s+admin\s+user\s+add[^\n]*(?:MINIO_APP_SECRET_KEY|\$\{?MINIO_APP_SECRET_KEY)`),
			entrypointSource, "the MinIO application secret must not enter process arguments")
	})

	t.Run("Media waits for bucket initialization", func(t *testing.T) {
		media, exists := compose.Services["media_server"]
		require.True(t, exists, "root docker-compose.yml must define media_server")
		require.Equal(t, "service_completed_successfully",
			dependencyCondition(media.DependsOn, "minio-init"),
			"Media must not start before the one-shot bucket initializer succeeds")
	})

	t.Run("Media uses service DNS for S3", func(t *testing.T) {
		media := compose.Services["media_server"]
		configSource, readOnly, found := findVolumeMount(media.Volumes, "/im/conf/media.json")
		require.True(t, found, "Media must mount its S3 configuration")
		configSource = strings.TrimPrefix(filepath.ToSlash(configSource), "./")
		mediaConfigBytes, err := os.ReadFile(filepath.Join(root, filepath.FromSlash(configSource)))
		require.NoError(t, err)
		var mediaConfig struct {
			S3 struct {
				Endpoint       string `json:"endpoint"`
				PublicEndpoint string `json:"public_endpoint"`
			} `json:"s3"`
		}
		require.NoError(t, json.Unmarshal(mediaConfigBytes, &mediaConfig))
		endpoint, err := url.Parse(mediaConfig.S3.Endpoint)
		require.NoError(t, err)
		require.Equal(t, "minio", endpoint.Hostname(),
			"Media S3 endpoint must resolve through the root Compose MinIO service")
		require.Equal(t, "9000", endpoint.Port())
		require.False(t, isLoopbackHostname(endpoint.Hostname()),
			"a container dependency endpoint must not use loopback")

		publicEndpoint, err := url.Parse(mediaConfig.S3.PublicEndpoint)
		require.NoError(t, err)
		require.True(t, isLoopbackHostname(publicEndpoint.Hostname()),
			"local/CI presigned URLs must be reachable from the host client")
		require.Equal(t, "19000", publicEndpoint.Port())
		require.NotEqual(t, endpoint.Host, publicEndpoint.Host,
			"internal S3 requests and client-facing presigned URLs need distinct endpoints")

		s3Client, err := os.ReadFile(filepath.Join(root, "common/infra/s3_client.hpp"))
		require.NoError(t, err)
		require.Contains(t, string(s3Client), "public_endpoint",
			"S3Client must support a client-facing presign endpoint")
		require.Contains(t, string(s3Client), "_presign_client",
			"presigning must not reuse the internal service-DNS client")
		require.True(t, readOnly, "Media configuration must be mounted read-only")

		conversationConfig, err := os.ReadFile(filepath.Join(root, "conf/docker/conversation_server.conf"))
		require.NoError(t, err)
		require.Contains(t, string(conversationConfig),
			"-public_url_prefix=http://127.0.0.1:19000/chatnow-media-public",
			"Conversation-generated public media URLs must use the client-facing MinIO endpoint")
		conversationSource, err := os.ReadFile(filepath.Join(root, "conversation/source/conversation_server.cc"))
		require.NoError(t, err)
		require.Contains(t, string(conversationSource),
			`"http://127.0.0.1:19000/chatnow-media-public"`)
	})

	t.Run("published host ports are unique", func(t *testing.T) {
		owners := make(map[string]string)
		for serviceName, service := range compose.Services {
			for _, rawPort := range service.Ports {
				hostPort, published := publishedHostPort(rawPort)
				if !published {
					continue
				}
				if previous, duplicate := owners[hostPort]; duplicate {
					t.Errorf("host port %s is published by both %s and %s", hostPort, previous, serviceName)
					continue
				}
				owners[hostPort] = serviceName
			}
		}
	})

	t.Run("Redis cluster initialization has no fixed sleep", func(t *testing.T) {
		initializer, exists := compose.Services["redis-cluster-init"]
		require.True(t, exists, "root docker-compose.yml must define redis-cluster-init")
		initializerText := serviceCommandText(initializer)
		require.NotRegexp(t, regexp.MustCompile(`(?i)\bsleep\s+[0-9]+(?:s)?\b`), initializerText,
			"a fixed sleep is not Redis Cluster readiness evidence")
	})

	t.Run("Redis cluster initializer exits", func(t *testing.T) {
		initializer, exists := compose.Services["redis-cluster-init"]
		require.True(t, exists, "root docker-compose.yml must define redis-cluster-init")
		initializerText := serviceCommandText(initializer)
		require.NotRegexp(t, regexp.MustCompile(`(?i)\btail\s+-f\b`), initializerText,
			"redis-cluster-init must exit after convergence")
	})

	t.Run("Redis cluster initialization is bounded", func(t *testing.T) {
		initializer, exists := compose.Services["redis-cluster-init"]
		require.True(t, exists, "root docker-compose.yml must define redis-cluster-init")
		initializerText := serviceCommandText(initializer)
		initializerScript, err := os.ReadFile(filepath.Join(root, "scripts/init_redis_cluster.sh"))
		require.NoError(t, err)
		initializerText += "\n" + string(initializerScript)
		require.True(t, declaresBoundedDeadline(initializerText),
			"redis-cluster-init must declare and enforce a bounded deadline or attempt limit")
	})

	t.Run("Redis Cluster advertises stable service DNS", func(t *testing.T) {
		for index := 1; index <= 6; index++ {
			serviceName := fmt.Sprintf("redis-node%d", index)
			node, exists := compose.Services[serviceName]
			require.True(t, exists)
			command := serviceCommandText(node)
			require.Contains(t, command, "--cluster-announce-hostname "+serviceName)
			require.Contains(t, command, "--cluster-preferred-endpoint-type hostname")
		}
	})

	t.Run("RabbitMQ persistent node identity is stable", func(t *testing.T) {
		rabbit, exists := compose.Services["rabbitmq"]
		require.True(t, exists)
		require.Equal(t, "rabbitmq", rabbit.Hostname,
			"RabbitMQ must keep a stable node identity across container recreation")
		nodeName, declared := environmentValue(rabbit.Environment, "RABBITMQ_NODENAME")
		require.True(t, declared)
		require.Equal(t, "rabbit@rabbitmq", nodeName)
	})

}

// Repository contract | P0 | RabbitMQ hosts are not expanded into invalid double-port URLs.
func TestRabbitMQRuntimeAddressContract(t *testing.T) {
	root := repositoryRoot(t)
	for _, relativePath := range []string{
		"conf/docker/message_server.conf",
		"conf/docker/push_server.conf",
		"conf/docker/transmite_server.conf",
		"conf/local/message_server.conf",
		"conf/local/push_server.conf",
		"conf/local/transmite_server.conf",
		"conf/transmite_server.conf.example",
		"message/source/message_server.cc",
		"push/source/push_server.cc",
		"transmite/source/transmite_server.cc",
	} {
		content, err := os.ReadFile(filepath.Join(root, relativePath))
		require.NoError(t, err)
		mqDefault := regexp.MustCompile(`DEFINE_string\s*\(\s*mq_host\s*,\s*"([^"]+)"`)
		for _, line := range strings.Split(string(content), "\n") {
			trimmed := strings.TrimSpace(line)
			host := ""
			if strings.HasPrefix(trimmed, "-mq_host=") {
				host = strings.TrimSpace(strings.TrimPrefix(trimmed, "-mq_host="))
			} else if match := mqDefault.FindStringSubmatch(line); len(match) == 2 {
				host = match[1]
			}
			if host == "" {
				continue
			}
			require.NotContains(t, host, ":",
				"%s must contain a host only; RabbitMQ builders append port 5672", relativePath)
		}
	}
}

// Repository contract | P0 | RabbitMQ credentials cannot change AMQP URI structure.
func TestRabbitMQRuntimeCredentialEncodingContract(t *testing.T) {
	root := repositoryRoot(t)
	helper, err := os.ReadFile(filepath.Join(root, "common/mq/amqp_url.hpp"))
	require.NoError(t, err, "a shared AMQP URI builder must encode credentials")
	helperSource := string(helper)
	require.Contains(t, helperSource, "percent_encode_userinfo")
	require.Contains(t, helperSource, "make_amqp_url")

	for _, relativePath := range []string{
		"message/source/message_server.h",
		"push/source/push_server.h",
		"transmite/source/transmite_server.h",
	} {
		content, err := os.ReadFile(filepath.Join(root, relativePath))
		require.NoError(t, err)
		require.Contains(t, string(content), "make_amqp_url(",
			"%s must not concatenate credentials into an AMQP URI", relativePath)
	}
}

// Repository contract | P0 | A clean MySQL volume creates every current ODB object table.
func TestComposeDatabaseBootstrapContract(t *testing.T) {
	root := repositoryRoot(t)
	compose := readRuntimeCompose(t, root)
	mysql, exists := compose.Services["mysql"]
	require.True(t, exists, "root docker-compose.yml must define mysql")

	t.Run("database name is declared", func(t *testing.T) {
		databaseName, declared := environmentValue(mysql.Environment, "MYSQL_DATABASE")
		require.True(t, declared, "MySQL must declare MYSQL_DATABASE")
		require.True(t, databaseName == "chatnow" || strings.HasSuffix(databaseName, ":-chatnow}"),
			"MYSQL_DATABASE must resolve to chatnow, got %q", databaseName)
	})

	t.Run("migration mount is read-only", func(t *testing.T) {
		initializer, exists := compose.Services["mysql-init"]
		require.True(t, exists, "root docker-compose.yml must define mysql-init")
		require.Equal(t, "service_healthy", dependencyCondition(initializer.DependsOn, "mysql"),
			"mysql-init must wait for MySQL health")
		require.Equal(t, "no", strings.ToLower(fmt.Sprint(initializer.Restart)),
			"mysql-init must be a one-shot service")
		_, readOnly, found := findVolumeMount(initializer.Volumes, "/migrations")
		require.True(t, found, "mysql-init must mount the versioned sql directory")
		require.True(t, readOnly, "the migration mount must be read-only")
	})

	t.Run("application services wait for schema convergence", func(t *testing.T) {
		for _, serviceName := range []string{
			"identity_server", "conversation_server", "relationship_server",
			"message_server", "media_server",
		} {
			service, exists := compose.Services[serviceName]
			require.True(t, exists)
			require.Equal(t, "service_completed_successfully",
				dependencyCondition(service.DependsOn, "mysql-init"),
				"%s must not start before schema and grants converge", serviceName)
		}
	})

	t.Run("migration runner rejects changed applied versions", func(t *testing.T) {
		path := filepath.Join(root, "scripts/init_mysql.sh")
		info, err := os.Stat(path)
		require.NoError(t, err, "a repeatable MySQL migration runner must exist")
		require.NotZero(t, info.Mode().Perm()&0o111, "MySQL migration runner must be executable")
		content, err := os.ReadFile(path)
		require.NoError(t, err)
		source := strings.ToLower(string(content))
		require.Contains(t, source, "schema_migrations")
		require.Contains(t, source, "sha256")
		require.Contains(t, source, "checksum")
		require.Regexp(t, regexp.MustCompile(`(?is)checksum.{0,500}(?:mismatch|changed).{0,500}exit\s+1`), source,
			"an applied migration whose checksum changes must fail closed")
	})

	t.Run("migrations cover current ODB object tables idempotently", func(t *testing.T) {
		objectTables := odbObjectTables(t, root)
		require.NotEmpty(t, objectTables)
		migrationTables := migrationTableContracts(t, root)

		var missing []string
		var nonIdempotent []string
		for _, table := range objectTables {
			idempotent, present := migrationTables[table]
			if !present {
				missing = append(missing, table)
				continue
			}
			if !idempotent {
				nonIdempotent = append(nonIdempotent, table)
			}
		}
		require.Empty(t, missing, "versioned migrations are missing current ODB object tables")
		require.Empty(t, nonIdempotent,
			"cold-start CREATE TABLE statements must use IF NOT EXISTS")
	})
}

// Repository contract | P0 | CI readiness has a bounded semantic helper.
func TestRuntimeReadinessHelperContract(t *testing.T) {
	root := repositoryRoot(t)
	path := filepath.Join(root, "scripts/wait_for_services.sh")
	info, err := os.Stat(path)
	require.NoError(t, err, "scripts/wait_for_services.sh must exist")
	require.True(t, info.Mode().IsRegular())
	require.NotZero(t, info.Mode().Perm()&0o111,
		"scripts/wait_for_services.sh must be executable")

	content, err := os.ReadFile(path)
	require.NoError(t, err)
	source := string(content)
	require.True(t, declaresBoundedDeadline(source),
		"readiness must declare and enforce a bounded deadline or attempt limit")

	probes := map[string]*regexp.Regexp{
		"Redis Cluster state": regexp.MustCompile(
			`(?is)redis-cli.{0,300}cluster\s+info.{0,300}cluster_state\s*:\s*ok`),
		"MySQL schema": regexp.MustCompile(
			`(?is)(?:mysql|mariadb).{0,300}(?:information_schema\.tables|show\s+tables)`),
		"RabbitMQ readiness": regexp.MustCompile(
			`(?is)(?:rabbitmq-diagnostics.{0,160}(?:ping|check_running)|/api/health/checks/(?:ready-to-serve-clients|alarms|local-alarms))`),
		"Elasticsearch health": regexp.MustCompile(`(?is)_cluster/health`),
		"MinIO health":         regexp.MustCompile(`(?is)/minio/health/(?:ready|live)`),
		"public MinIO bucket": regexp.MustCompile(
			`(?is)(?:mc|mcli)\s+(?:stat|ls).{0,240}chatnow-media-public`),
		"private MinIO bucket": regexp.MustCompile(
			`(?is)(?:mc|mcli)\s+(?:stat|ls).{0,240}chatnow-media-private`),
		"etcd service registrations": regexp.MustCompile(
			`(?is)(?:etcdctl.{0,240}get.{0,240}(?:--prefix.{0,120}/service|/service.{0,120}--prefix)|/v3/kv/range)`),
		"Gateway HTTP readiness": regexp.MustCompile(
			`(?is)(?:curl|wget).{0,240}(?:gateway|127\.0\.0\.1|localhost).{0,160}/health`),
		"Push service readiness": regexp.MustCompile(
			`(?is)(?:nc|curl|wget|websocat).{0,240}(?:push|127\.0\.0\.1|localhost).{0,160}(?:9001|health|ready)`),
	}
	for name, pattern := range probes {
		require.True(t, pattern.MatchString(source), "readiness helper must probe %s", name)
	}
}

// Repository contract | P0 | Gateway health is dependency-aware, not liveness-only.
func TestGatewayHealthReadinessContract(t *testing.T) {
	root := repositoryRoot(t)
	header, err := os.ReadFile(filepath.Join(root, "gateway/source/gateway_server.h"))
	require.NoError(t, err)
	implementation, err := os.ReadFile(filepath.Join(root, "gateway/source/gateway_server.cc"))
	require.NoError(t, err)
	source := string(header) + "\n" + string(implementation)

	registration := regexp.MustCompile(`(?s)_http_server\s*\.\s*Get\s*\(\s*"/health"`)
	require.True(t, registration.MatchString(source), "Gateway must register GET /health")

	healthOffset := strings.Index(source, `"/health"`)
	require.GreaterOrEqual(t, healthOffset, 0)
	healthEnd := healthOffset + 3000
	if healthEnd > len(source) {
		healthEnd = len(source)
	}
	healthContract := source[healthOffset:healthEnd]
	require.True(t,
		regexp.MustCompile(`(?i)(ready|readiness|dependenc|service_manager|_channels|redis|etcd)`).MatchString(healthContract),
		"Gateway health must inspect dependencies")
	require.True(t,
		regexp.MustCompile(`(?i)(status\s*=\s*50[023]|service_unavailable|unavailable)`).MatchString(healthContract),
		"Gateway health must return a non-200 result when dependencies are unavailable")
}

func readRuntimeCompose(t testing.TB, root string) composeRuntimeDocument {
	t.Helper()
	content, err := os.ReadFile(filepath.Join(root, "docker-compose.yml"))
	require.NoError(t, err)
	var compose composeRuntimeDocument
	require.NoError(t, yaml.Unmarshal(content, &compose), "docker-compose.yml must be valid YAML")
	require.NotEmpty(t, compose.Services)
	return compose
}

func serviceCommandText(service composeRuntimeService) string {
	return strings.TrimSpace(stringifyYAMLValue(service.Entrypoint) + "\n" + stringifyYAMLValue(service.Command))
}

func stringifyYAMLValue(value any) string {
	switch typed := value.(type) {
	case nil:
		return ""
	case string:
		return typed
	case []any:
		parts := make([]string, 0, len(typed))
		for _, item := range typed {
			parts = append(parts, stringifyYAMLValue(item))
		}
		return strings.Join(parts, " ")
	default:
		return fmt.Sprint(typed)
	}
}

func dependencyCondition(raw any, dependency string) string {
	switch typed := raw.(type) {
	case []any:
		for _, candidate := range typed {
			if fmt.Sprint(candidate) == dependency {
				return "service_started"
			}
		}
	case map[string]any:
		value, exists := typed[dependency]
		if !exists {
			return ""
		}
		if details, ok := value.(map[string]any); ok {
			return fmt.Sprint(details["condition"])
		}
		return "service_started"
	}
	return ""
}

func publishedHostPort(raw any) (string, bool) {
	if details, ok := raw.(map[string]any); ok {
		published := strings.TrimSpace(fmt.Sprint(details["published"]))
		return published, published != "" && published != "<nil>"
	}
	short := strings.TrimSpace(fmt.Sprint(raw))
	short = strings.TrimSuffix(strings.TrimSuffix(short, "/tcp"), "/udp")
	lastColon := strings.LastIndex(short, ":")
	if lastColon < 0 {
		return "", false
	}
	hostSide := short[:lastColon]
	if strings.HasPrefix(hostSide, "${") {
		return hostSide, true
	}
	if hostColon := strings.LastIndex(hostSide, ":"); hostColon >= 0 {
		hostSide = hostSide[hostColon+1:]
	}
	hostSide = strings.Trim(hostSide, "[]")
	return hostSide, hostSide != ""
}

func findVolumeMount(rawMounts []any, target string) (source string, readOnly bool, found bool) {
	normalizedTarget := strings.TrimRight(target, "/")
	for _, raw := range rawMounts {
		if details, ok := raw.(map[string]any); ok {
			if strings.TrimRight(fmt.Sprint(details["target"]), "/") != normalizedTarget {
				continue
			}
			readOnly, _ = details["read_only"].(bool)
			return fmt.Sprint(details["source"]), readOnly, true
		}

		short := fmt.Sprint(raw)
		parts := strings.Split(short, ":")
		if len(parts) < 2 || strings.TrimRight(parts[1], "/") != normalizedTarget {
			continue
		}
		for _, option := range parts[2:] {
			if option == "ro" {
				readOnly = true
			}
		}
		return parts[0], readOnly, true
	}
	return "", false, false
}

func environmentValue(raw any, name string) (string, bool) {
	switch typed := raw.(type) {
	case map[string]any:
		value, exists := typed[name]
		if !exists {
			return "", false
		}
		return fmt.Sprint(value), true
	case []any:
		prefix := name + "="
		for _, item := range typed {
			candidate := fmt.Sprint(item)
			if strings.HasPrefix(candidate, prefix) {
				return strings.TrimPrefix(candidate, prefix), true
			}
		}
	}
	return "", false
}

func odbObjectTables(t testing.TB, root string) []string {
	t.Helper()
	paths, err := filepath.Glob(filepath.Join(root, "odb/*.hxx"))
	require.NoError(t, err)
	require.NotEmpty(t, paths)
	objectPragma := regexp.MustCompile(`#pragma\s+db\s+object\s+table\("([A-Za-z_][A-Za-z0-9_]*)"\)`)
	set := make(map[string]struct{})
	for _, path := range paths {
		content, err := os.ReadFile(path)
		require.NoError(t, err)
		for _, match := range objectPragma.FindAllStringSubmatch(string(content), -1) {
			set[match[1]] = struct{}{}
		}
	}
	tables := make([]string, 0, len(set))
	for table := range set {
		tables = append(tables, table)
	}
	sort.Strings(tables)
	return tables
}

func migrationTableContracts(t testing.TB, root string) map[string]bool {
	t.Helper()
	paths, err := filepath.Glob(filepath.Join(root, "sql/*.sql"))
	require.NoError(t, err)
	require.NotEmpty(t, paths, "sql/ must contain versioned migrations")
	versionedName := regexp.MustCompile(`^V[0-9]+__[A-Za-z0-9_.-]+\.sql$`)
	createTable := regexp.MustCompile("(?i)CREATE\\s+TABLE\\s+(IF\\s+NOT\\s+EXISTS\\s+)?(?:\\x60?[A-Za-z_][A-Za-z0-9_]*\\x60?\\.)?\\x60?([A-Za-z_][A-Za-z0-9_]*)\\x60?")
	contracts := make(map[string]bool)
	for _, path := range paths {
		require.Regexp(t, versionedName, filepath.Base(path),
			"migration filenames must be versioned")
		content, err := os.ReadFile(path)
		require.NoError(t, err)
		for _, match := range createTable.FindAllStringSubmatch(string(content), -1) {
			idempotent := strings.TrimSpace(match[1]) != ""
			if previous, exists := contracts[match[2]]; exists {
				contracts[match[2]] = previous && idempotent
			} else {
				contracts[match[2]] = idempotent
			}
		}
	}
	return contracts
}

func declaresBoundedDeadline(source string) bool {
	boundDeclaration := regexp.MustCompile(`(?im)\b[A-Z][A-Z0-9_]*(?:TIMEOUT|DEADLINE|MAX_ATTEMPTS|MAX_RETRIES)[A-Z0-9_]*\s*=`)
	boundEnforcement := regexp.MustCompile(`(?im)(date\s+\+%s|\bSECONDS\b|\btimeout\b|\battempts?\b|\bretr(?:y|ies)\b)`)
	return boundDeclaration.MatchString(source) && boundEnforcement.MatchString(source)
}

func isLoopbackHostname(host string) bool {
	if strings.EqualFold(host, "localhost") {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}
