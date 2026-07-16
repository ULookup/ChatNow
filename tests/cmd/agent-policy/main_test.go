package main

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestRun(t *testing.T) {
	tests := []struct {
		name       string
		args       []string
		event      string
		wantCode   int
		wantStdout string
		wantStderr string
	}{
		{
			name:       "unknown command is usage error",
			args:       []string{"unknown"},
			wantCode:   2,
			wantStderr: "unknown command",
		},
		{
			name:       "missing event is input error",
			args:       []string{"issue"},
			wantCode:   2,
			wantStderr: "GITHUB_EVENT_PATH",
		},
		{
			name:     "valid Issue event",
			args:     []string{"issue"},
			event:    issueEventJSON("Validate policy events"),
			wantCode: 0,
		},
		{
			name:       "violation uses GitHub annotation",
			args:       []string{"issue"},
			event:      issueEventJSON("无效标题"),
			wantCode:   1,
			wantStdout: "::error title=ISSUE_TITLE_ENGLISH_REQUIRED::",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			environment := map[string]string{}
			if tt.event != "" {
				eventFile := filepath.Join(t.TempDir(), "event.json")
				if err := os.WriteFile(eventFile, []byte(tt.event), 0o644); err != nil {
					t.Fatal(err)
				}
				environment["GITHUB_EVENT_PATH"] = eventFile
			}
			getenv := func(key string) string { return environment[key] }
			var stdout, stderr bytes.Buffer

			if got := run(tt.args, getenv, &stdout, &stderr); got != tt.wantCode {
				t.Fatalf("run() code = %d, want %d; stdout=%q stderr=%q", got, tt.wantCode, stdout.String(), stderr.String())
			}
			if !strings.Contains(stdout.String(), tt.wantStdout) {
				t.Errorf("stdout = %q, want substring %q", stdout.String(), tt.wantStdout)
			}
			if !strings.Contains(stderr.String(), tt.wantStderr) {
				t.Errorf("stderr = %q, want substring %q", stderr.String(), tt.wantStderr)
			}
		})
	}
}

func issueEventJSON(title string) string {
	body := `## Target Version
3.0-dev

## Evidence
当前策略缺少事件适配验证。

## Problem or Goal
验证策略命令读取事件。

## Scope
只覆盖命令适配层。

## Non-goals
不修改生产服务。

## Acceptance Criteria
有效事件返回成功状态。

## Test-first Plan
先观察命令测试失败。

## Risk and Security
None：不读取凭据或生产数据。

## Architecture Impact
No，因为服务边界不变。

## Core-flow Impact
No，因为业务流程不变。

## Required Skill Updates
None：命令实现不改变技能规范。
`
	return `{"issue":{"title":` + quoteJSON(title) + `,"body":` + quoteJSON(body) + `,"labels":[]}}`
}

func quoteJSON(value string) string {
	var output strings.Builder
	output.WriteByte('"')
	for _, r := range value {
		switch r {
		case '\\':
			output.WriteString(`\\`)
		case '"':
			output.WriteString(`\"`)
		case '\n':
			output.WriteString(`\n`)
		case '\r':
			output.WriteString(`\r`)
		case '\t':
			output.WriteString(`\t`)
		default:
			output.WriteRune(r)
		}
	}
	output.WriteByte('"')
	return output.String()
}
