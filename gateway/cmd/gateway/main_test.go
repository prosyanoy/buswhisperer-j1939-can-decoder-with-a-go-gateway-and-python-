package main

import (
	"testing"
	"time"
)

func TestLoadConfigDefaults(t *testing.T) {
	for _, k := range []string{"BW_LOG", "BW_SINK", "BW_OUT", "BW_BATCH_SIZE", "BW_FLUSH_INTERVAL_MS", "BW_HTTP_RETRIES"} {
		t.Setenv(k, "")
	}
	c, err := loadConfig()
	if err != nil {
		t.Fatal(err)
	}
	if c.Sink != "file" || c.BatchSize != 100 || c.FlushInterval != time.Second || c.Log != "data/sample.log" {
		t.Fatalf("%+v", c)
	}
}

func TestLoadConfigErrors(t *testing.T) {
	cases := map[string]string{"BW_BATCH_SIZE": "0", "BW_FLUSH_INTERVAL_MS": "x", "BW_HTTP_RETRIES": "-1", "BW_SINK": "kafka"}
	for k, v := range cases {
		t.Run(k, func(t *testing.T) {
			t.Setenv(k, v)
			if _, err := loadConfig(); err == nil {
				t.Fatalf("expected error for %s=%s", k, v)
			}
		})
	}
}
