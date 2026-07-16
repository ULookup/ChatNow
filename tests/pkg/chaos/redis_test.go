package chaos

import "testing"

func TestClusterHealthyRequiresCompleteSlotCoverage(t *testing.T) {
	healthy := "cluster_state:ok\r\ncluster_slots_assigned:16384\r\ncluster_slots_ok:16384\r\ncluster_slots_fail:0\r\n"
	if !clusterHealthy(healthy) {
		t.Fatal("complete healthy cluster rejected")
	}
	for _, unhealthy := range []string{
		"cluster_state:fail\ncluster_slots_assigned:16384\ncluster_slots_ok:16384\ncluster_slots_fail:0\n",
		"cluster_state:ok\ncluster_slots_assigned:12000\ncluster_slots_ok:12000\ncluster_slots_fail:0\n",
		"cluster_state:ok\ncluster_slots_assigned:16384\ncluster_slots_ok:16000\ncluster_slots_fail:384\n",
	} {
		if clusterHealthy(unhealthy) {
			t.Fatalf("unhealthy cluster accepted: %q", unhealthy)
		}
	}
}
