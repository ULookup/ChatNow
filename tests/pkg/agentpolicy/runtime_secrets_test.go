package agentpolicy

import (
	"bufio"
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"testing"
)

type runtimeSecretFinding struct {
	path     string
	line     int
	category string
}

var (
	defineStringAssignment = regexp.MustCompile(`DEFINE_string\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*"((?:\\.|[^"\\])*)"`)
	flagAssignment         = regexp.MustCompile(`^\s*-?([A-Za-z_][A-Za-z0-9_.-]*)\s*=\s*(.*?)\s*$`)
	yamlAssignment         = regexp.MustCompile(`^\s*-?\s*([A-Za-z_][A-Za-z0-9_.-]*)\s*[:=]\s*(.*?)\s*$`)
	jsonStringAssignment   = regexp.MustCompile(`^\s*"([^"]+)"\s*:\s*"((?:\\.|[^"\\])*)"`)
	environmentReference   = regexp.MustCompile(`^\$\$?\{[A-Za-z_][A-Za-z0-9_]*(?::[-?][^}]*)?\}$`)
	secretResolverCall     = regexp.MustCompile(`resolve_secret\s*\(\s*(?:[A-Za-z_][A-Za-z0-9_:]*::)?SecretId::`)
	directGetenvCall       = regexp.MustCompile(`\b(?:std::)?getenv\s*\(`)
)

const unifiedSecretResolverPath = "common/config/secret_resolver.hpp"

var secretConsumerPaths = []string{
	"conversation/source/conversation_server.cc",
	"gateway/source/gateway_server.cc",
	"identity/source/identity_server.cc",
	"media/source/media_main.cc",
	"message/source/message_server.cc",
	"push/source/push_server.cc",
	"relationship/source/relationship_server.cc",
	"transmite/source/transmite_server.cc",
}

// Repository policy | P0 | Tracked runtime secrets must use an explicit placeholder or injection reference.
func TestRepositoryRejectsTrackedRuntimeSecrets(t *testing.T) {
	root := repositoryRoot(t)
	paths := trackedRepositoryFiles(t, root)

	var findings []runtimeSecretFinding
	for _, path := range paths {
		if !isRuntimeSecretPolicyFile(path) {
			continue
		}
		findings = append(findings, scanTrackedRuntimeSecrets(t, root, path)...)
	}
	findings = append(findings, validateSecretInjectionContract(root)...)

	sort.Slice(findings, func(i, j int) bool {
		if findings[i].path != findings[j].path {
			return findings[i].path < findings[j].path
		}
		if findings[i].line != findings[j].line {
			return findings[i].line < findings[j].line
		}
		return findings[i].category < findings[j].category
	})
	for _, finding := range findings {
		if finding.line > 0 {
			t.Errorf("%s:%d: repository secret contract violation category=%s",
				finding.path, finding.line, finding.category)
			continue
		}
		t.Errorf("%s: repository secret contract violation category=%s",
			finding.path, finding.category)
	}
}

func validateSecretInjectionContract(root string) []runtimeSecretFinding {
	var findings []runtimeSecretFinding
	resolver, err := os.ReadFile(filepath.Join(root, filepath.FromSlash(unifiedSecretResolverPath)))
	if err != nil {
		category := "unified_secret_resolver_unreadable"
		if os.IsNotExist(err) {
			category = "unified_secret_resolver_missing"
		}
		findings = append(findings, runtimeSecretFinding{
			path:     unifiedSecretResolverPath,
			category: category,
		})
	} else {
		resolverText := string(resolver)
		checks := []struct {
			category string
			pattern  *regexp.Regexp
		}{
			{category: "secret_id_allowlist_missing", pattern: regexp.MustCompile(`enum\s+class\s+SecretId\b`)},
			{category: "secret_spec_allowlist_missing", pattern: regexp.MustCompile(`struct\s+SecretSpec\b`)},
			{category: "secret_resolver_api_missing", pattern: regexp.MustCompile(`resolve_secret\s*\(\s*SecretId\b`)},
			{category: "secret_env_source_missing", pattern: regexp.MustCompile(`\b(?:std::)?getenv\s*\(`)},
			{category: "secret_env_file_locator_missing", pattern: regexp.MustCompile(`\benv_file\b`)},
			{category: "secret_file_source_missing", pattern: regexp.MustCompile(`::open\s*\(`)},
			{category: "secret_file_no_follow_missing", pattern: regexp.MustCompile(`\bO_NOFOLLOW\b`)},
			{category: "secret_file_close_on_exec_missing", pattern: regexp.MustCompile(`\bO_CLOEXEC\b`)},
			{category: "secret_file_stat_missing", pattern: regexp.MustCompile(`::fstat\s*\(`)},
			{category: "secret_file_regular_check_missing", pattern: regexp.MustCompile(`\bS_ISREG\s*\(`)},
			{category: "secret_file_owner_check_missing", pattern: regexp.MustCompile(`\bst_uid\b`)},
			{category: "secret_file_permission_check_missing", pattern: regexp.MustCompile(`\bS_IRWXG\b.*\bS_IRWXO\b`)},
			{category: "secret_value_bound_missing", pattern: regexp.MustCompile(`\bmax_bytes\b`)},
			{category: "secret_nul_rejection_missing", pattern: regexp.MustCompile(`reject_nul`)},
			{category: "secret_trailing_line_trim_missing", pattern: regexp.MustCompile(`trim_one_trailing_line_ending`)},
			{category: "secret_source_conflict_check_missing", pattern: regexp.MustCompile(`source_conflict`)},
		}
		for _, check := range checks {
			if !check.pattern.MatchString(resolverText) {
				findings = append(findings, runtimeSecretFinding{
					path:     unifiedSecretResolverPath,
					category: check.category,
				})
			}
		}
	}

	for _, path := range secretConsumerPaths {
		content, err := os.ReadFile(filepath.Join(root, filepath.FromSlash(path)))
		if err != nil {
			findings = append(findings, runtimeSecretFinding{
				path:     path,
				category: "secret_consumer_contract_unreadable",
			})
			continue
		}
		if !secretResolverCall.Match(content) {
			findings = append(findings, runtimeSecretFinding{
				path:     path,
				category: "unified_secret_resolver_call_missing",
			})
		}
		for index, line := range strings.Split(string(content), "\n") {
			if directGetenvCall.MatchString(line) {
				findings = append(findings, runtimeSecretFinding{
					path:     path,
					line:     index + 1,
					category: "direct_secret_environment_access",
				})
			}
		}
	}
	return findings
}

func trackedRepositoryFiles(t *testing.T, root string) []string {
	t.Helper()
	cmd := exec.Command("git", "-C", root, "ls-files", "-z")
	output, err := cmd.Output()
	if err != nil {
		t.Fatalf("list tracked repository files: %v", err)
	}

	entries := bytes.Split(output, []byte{0})
	paths := make([]string, 0, len(entries))
	for _, entry := range entries {
		if len(entry) != 0 {
			paths = append(paths, filepath.ToSlash(string(entry)))
		}
	}
	return paths
}

func isRuntimeSecretPolicyFile(path string) bool {
	if path == "tests/config.yaml" {
		return true
	}
	if strings.HasPrefix(path, "tests/") {
		return false
	}

	ext := strings.ToLower(filepath.Ext(path))
	switch ext {
	case ".cc", ".cpp", ".h", ".hpp":
		return true
	case ".conf", ".json":
		return strings.HasPrefix(path, "conf/")
	case ".yml", ".yaml":
		name := strings.ToLower(filepath.Base(path))
		return strings.Contains(name, "compose")
	default:
		return false
	}
}

func scanTrackedRuntimeSecrets(t *testing.T, root, path string) []runtimeSecretFinding {
	t.Helper()
	file, err := os.Open(filepath.Join(root, filepath.FromSlash(path)))
	if err != nil {
		t.Fatalf("open tracked policy input %s: %v", path, err)
	}
	defer file.Close()

	var findings []runtimeSecretFinding
	scanner := bufio.NewScanner(file)
	lineNumber := 0
	jwtKeysDepth := -1
	jsonDepth := 0
	for scanner.Scan() {
		lineNumber++
		line := scanner.Text()
		field, value, ok := runtimeSecretAssignment(path, line)
		category := runtimeSecretCategory(path, field)
		if jwtKeysDepth >= 0 && jsonDepth > jwtKeysDepth && jsonStringAssignment.MatchString(line) {
			category = "jwt_signing_key"
		}
		if ok && category != "" && !isAllowedSecretReference(value) {
			findings = append(findings, runtimeSecretFinding{
				path:     path,
				line:     lineNumber,
				category: category,
			})
		}

		if strings.HasSuffix(strings.ToLower(path), ".json") {
			if jwtKeysDepth < 0 && strings.Contains(line, `"keys"`) && strings.Contains(line, "{") &&
				strings.Contains(strings.ToLower(path), "auth") {
				jwtKeysDepth = jsonDepth
			}
			jsonDepth += strings.Count(line, "{") - strings.Count(line, "}")
			if jwtKeysDepth >= 0 && jsonDepth <= jwtKeysDepth {
				jwtKeysDepth = -1
			}
		}
	}
	if err := scanner.Err(); err != nil {
		t.Fatalf("scan tracked policy input %s: %v", path, err)
	}
	return findings
}

func runtimeSecretAssignment(path, line string) (field, value string, ok bool) {
	ext := strings.ToLower(filepath.Ext(path))
	var match []string
	switch ext {
	case ".cc", ".cpp", ".h", ".hpp":
		match = defineStringAssignment.FindStringSubmatch(line)
	case ".conf":
		match = flagAssignment.FindStringSubmatch(line)
	case ".json":
		match = jsonStringAssignment.FindStringSubmatch(line)
	case ".yml", ".yaml":
		match = yamlAssignment.FindStringSubmatch(line)
	}
	if len(match) != 3 {
		return "", "", false
	}
	return match[1], match[2], true
}

func runtimeSecretCategory(path, field string) string {
	name := strings.ToLower(strings.ReplaceAll(strings.ReplaceAll(field, "-", "_"), ".", "_"))
	passwordField := strings.Contains(name, "password") || strings.Contains(name, "passwd") ||
		strings.Contains(name, "pswd") || strings.Contains(name, "paswd") || strings.HasSuffix(name, "_pass")

	switch {
	case path == "tests/config.yaml" && name == "mysql_dsn":
		return "database_password"
	case passwordField && containsAny(name, "mysql", "database", "db_"):
		return "database_password"
	case passwordField && containsAny(name, "smtp", "mail"):
		return "smtp_password"
	case passwordField && containsAny(name, "rabbit", "amqp", "mq_"):
		return "message_broker_password"
	case passwordField && strings.Contains(name, "redis"):
		return "redis_password"
	case strings.Contains(name, "jwt") && containsAny(name, "secret", "signing", "private_key"):
		return "jwt_signing_key"
	case strings.Contains(name, "minio") && containsAny(name, "root_user", "access_key"):
		return "object_storage_access_key"
	case strings.Contains(name, "minio") && containsAny(name, "password", "secret_key"):
		return "object_storage_secret_key"
	case strings.HasPrefix(path, "conf/") && strings.Contains(strings.ToLower(path), "media") && name == "access_key":
		return "object_storage_access_key"
	case strings.HasPrefix(path, "conf/") && strings.Contains(strings.ToLower(path), "media") && name == "secret_key":
		return "object_storage_secret_key"
	case passwordField && (strings.HasPrefix(path, "conf/") || strings.Contains(strings.ToLower(filepath.Base(path)), "compose")):
		return "runtime_password"
	default:
		return ""
	}
}

func isAllowedSecretReference(raw string) bool {
	value := strings.TrimSpace(raw)
	if comment := strings.Index(value, " #"); comment >= 0 {
		value = strings.TrimSpace(value[:comment])
	}
	value = strings.Trim(value, `"'`)
	if value == "" || environmentReference.MatchString(value) {
		return true
	}

	lower := strings.ToLower(value)
	if strings.HasPrefix(lower, "file:") || strings.HasPrefix(lower, "/run/secrets/") ||
		strings.HasPrefix(lower, "${") || strings.HasPrefix(lower, "$${") {
		return true
	}
	return containsAny(lower,
		"<placeholder>", "placeholder-only", "example-only", "dummy-only",
		"test-only", "local-dev-only", "not-a-secret", "replace-me")
}

func containsAny(value string, candidates ...string) bool {
	for _, candidate := range candidates {
		if strings.Contains(value, candidate) {
			return true
		}
	}
	return false
}
