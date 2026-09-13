package contracts

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
	"gopkg.in/yaml.v3"
)

// Process boundary | P0 | Diagnostic artifacts retain state, never raw logs or secrets.
func TestRuntimeDiagnosticsAreBoundedAndRedacted(t *testing.T) {
	const secret = "synthetic-private-value"
	directory := t.TempDir()
	const docker = `#!/usr/bin/env python3
import base64,json,os,sys,time
args=sys.argv[1:]
if os.getenv('DIAGNOSTIC_STALL') == '1': time.sleep(4)
if args[0] == 'inspect':
 print(json.dumps({'State':{'Status':'running','Running':True,'Paused':False,'Restarting':False,'OOMKilled':False,'ExitCode':0,'Pid':123,'StartedAt':'2026-09-13T00:00:00Z','FinishedAt':'0001-01-01T00:00:00Z','Error':'synthetic-private-value'},'RestartCount':2}))
elif 'ps' in args: print('a'*64)
elif 'etcdctl' in args:
 print(json.dumps({'kvs':[{'key':base64.b64encode(b'/service/message_service/instance').decode(),'value':base64.b64encode(b'synthetic-private-value').decode(),'lease':123}]}))
elif 'tail' in args:
 print(json.dumps({'ts':'2026-09-13T00:00:00Z','trace_id':'a'*32,'user_id':'synthetic-private-value','device_id':'synthetic-private-value','msg':'Gateway RPC failed err=[112] [E112]not connected synthetic-private-value','fields':{'file':'gateway_server.h','line':'134'}}))
 print(json.dumps({'msg':'rpc_failed code=4001 msg=mid not found synthetic-private-value'}))
 print(json.dumps({'msg':'password=synthetic-private-value'}))
else:
 print('synthetic-private-value',file=sys.stderr)
 sys.exit(42)
`
	require.NoError(t, os.WriteFile(filepath.Join(directory, "docker"), []byte(docker), 0700))
	for _, stalled := range []bool{false, true} {
		t.Run(map[bool]string{false: "snapshot", true: "deadline"}[stalled], func(t *testing.T) {
			output := filepath.Join(t.TempDir(), "snapshot.json")
			args := []string{filepath.Join(repositoryRoot(t), "scripts/collect_test_diagnostics.py"), "--output", output}
			if stalled {
				args = append(args, "--timeout-sec", "0.2")
			}
			cmd := exec.Command("python3", args...)
			cmd.Env = append(os.Environ(), "PATH="+directory+string(os.PathListSeparator)+os.Getenv("PATH"))
			if stalled {
				cmd.Env = append(cmd.Env, "DIAGNOSTIC_STALL=1")
			}
			started := time.Now()
			stdout, err := cmd.CombinedOutput()
			require.NoError(t, err, string(stdout))
			data, err := os.ReadFile(output)
			require.NoError(t, err)
			require.NotContains(t, string(data)+string(stdout), secret)
			var snapshot map[string]any
			require.NoError(t, json.Unmarshal(data, &snapshot))
			if stalled {
				require.Less(t, time.Since(started), 2*time.Second)
				require.Contains(t, string(data), "deadline")
			} else {
				require.Contains(t, string(data), `"restart_count": 2`)
				require.Contains(t, string(data), `"registered": true`)
				require.Contains(t, string(data), `"rpc_code": 112`)
				require.Contains(t, string(data), `"error_code": 4001`)
			}
		})
	}
}

func TestCorrectnessGatesPreserveFailureDiagnostics(t *testing.T) {
	data, err := os.ReadFile(filepath.Join(repositoryRoot(t), ".github/workflows/ci.yml"))
	require.NoError(t, err)
	var workflow workflowContract
	require.NoError(t, yaml.Unmarshal(data, &workflow))
	for _, name := range []string{"bvt", "func", "reliability"} {
		job := workflow.Jobs[name]
		before, failure, upload := false, false, false
		for _, step := range job.Steps {
			if strings.Contains(step.Run, "collect_test_diagnostics.py") {
				before = before || (step.If == "" && strings.Contains(step.Run, "before.json"))
				failure = failure || (step.If == "failure()" && strings.Contains(step.Run, "failure.json"))
			}
			upload = upload || (step.If == "failure()" && strings.HasPrefix(step.Uses, "actions/upload-artifact@") && step.With["path"] == "test-diagnostics")
		}
		require.True(t, before && failure && upload, "%s must retain before/failure state and a sanitized artifact", name)
	}
}
