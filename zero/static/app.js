(() => {
  "use strict";

  const state = {
    status: null,
    recordings: [],
  };

  const els = {
    connectionBanner: document.getElementById("connection-banner"),
    stateBadge: document.getElementById("state-badge"),
    activeName: document.getElementById("active-name"),
    lastError: document.getElementById("last-error"),
    errorBanner: document.getElementById("error-banner"),
    underrunBanner: document.getElementById("underrun-banner"),
    diagnostics: document.getElementById("diagnostics"),
    diagQueuedCount: document.getElementById("diag-queued-count"),
    diagFreeCapacity: document.getElementById("diag-free-capacity"),
    diagRecordCount: document.getElementById("diag-record-count"),
    diagProgress: document.getElementById("diag-progress"),
    diagProgressText: document.getElementById("diag-progress-text"),
    diagUnderrunCount: document.getElementById("diag-underrun-count"),
    stopAllBtn: document.getElementById("stop-all-btn"),
    recordForm: document.getElementById("record-form"),
    recordName: document.getElementById("record-name"),
    recordStopBtn: document.getElementById("record-stop-btn"),
    recordingsBody: document.querySelector("#recordings-table tbody"),
    rowTemplate: document.getElementById("recording-row-template"),
  };

  async function apiFetch(path, options) {
    const response = await fetch(path, options);
    let body = null;
    try {
      body = await response.json();
    } catch (error) {
      body = null;
    }
    if (!response.ok) {
      const message = (body && body.error) || `request failed with status ${response.status}`;
      throw new Error(message);
    }
    return body;
  }

  function formatDuration(durationUs) {
    if (!durationUs) return "0 s";
    return `${(durationUs / 1_000_000).toFixed(2)} s`;
  }

  function renderStatus(status) {
    state.status = status;
    els.stateBadge.textContent = status.state;
    els.stateBadge.className = `badge state-${status.state}`;
    els.activeName.textContent = status.active ? `${status.active} (${status.name})` : "-";
    els.lastError.textContent = status.last_error || "-";

    if (status.state === "ERROR") {
      els.errorBanner.hidden = false;
      els.errorBanner.textContent = `ERROR${status.last_error ? `: ${status.last_error}` : ""} -- use Return to PASS to recover.`;
    } else {
      els.errorBanner.hidden = true;
    }

    const diagnostics = status.diagnostics || { buffer: null, playback: null, recording: null };
    const hasDiagnostics = diagnostics.buffer || diagnostics.playback || diagnostics.recording;
    els.diagnostics.hidden = !hasDiagnostics;

    if (diagnostics.buffer) {
      els.diagQueuedCount.textContent = diagnostics.buffer.queued_count;
      els.diagFreeCapacity.textContent = diagnostics.buffer.free_capacity;
    } else {
      els.diagQueuedCount.textContent = "-";
      els.diagFreeCapacity.textContent = "-";
    }

    els.diagRecordCount.textContent = diagnostics.recording ? diagnostics.recording.event_count : "-";

    if (diagnostics.playback) {
      const playback = diagnostics.playback;
      els.diagProgress.max = Math.max(playback.duration_us, 1);
      els.diagProgress.value = playback.elapsed_us_estimate;
      els.diagProgressText.textContent =
        `${playback.queued_events}/${playback.total_events} events, ` +
        `~${formatDuration(playback.elapsed_us_estimate)} / ${formatDuration(playback.duration_us)}`;
      els.diagUnderrunCount.textContent = playback.underrun_count;
      if (playback.underrun_count > 0) {
        els.underrunBanner.hidden = false;
        els.underrunBanner.textContent = `${playback.underrun_count} underrun(s) detected during playback.`;
      } else {
        els.underrunBanner.hidden = true;
      }
    } else {
      els.diagProgress.value = 0;
      els.diagProgressText.textContent = "-";
      if (!status.last_result || !status.last_result.underruns || status.last_result.underruns.length === 0) {
        els.underrunBanner.hidden = true;
      }
    }

    renderRecordings();
  }

  function renderRecordings() {
    els.recordingsBody.innerHTML = "";
    const activeName = state.status && state.status.active ? state.status.name : null;
    for (const recording of state.recordings) {
      const row = els.rowTemplate.content.firstElementChild.cloneNode(true);
      row.querySelector(".rec-name").textContent = recording.name;
      row.querySelector(".rec-duration").textContent = formatDuration(recording.duration_us);
      row.querySelector(".rec-events").textContent = recording.event_count;

      const isActive = recording.name === activeName;
      const isPlaying = isActive && state.status.active === "playback";
      row.querySelector('[data-action="play"]').hidden = isActive;
      row.querySelector('[data-action="abort"]').hidden = !isPlaying;
      row.querySelector('[data-action="rename"]').disabled = isActive;
      row.querySelector('[data-action="delete"]').disabled = isActive;

      row.querySelector('[data-action="play"]').addEventListener("click", () => playRecording(recording.name));
      row.querySelector('[data-action="abort"]').addEventListener("click", () => abortPlayback());
      row.querySelector('[data-action="download"]').addEventListener("click", () => downloadRecording(recording.name));
      row.querySelector('[data-action="rename"]').addEventListener("click", () => renameRecording(recording.name));
      row.querySelector('[data-action="delete"]').addEventListener("click", () => deleteRecording(recording.name));

      els.recordingsBody.appendChild(row);
    }
  }

  async function loadRecordings() {
    try {
      const body = await apiFetch("/api/recordings");
      state.recordings = body.recordings;
      renderRecordings();
    } catch (error) {
      console.error("failed to load recordings", error);
    }
  }

  async function reportAction(promise) {
    try {
      await promise;
      await loadRecordings();
    } catch (error) {
      window.alert(error.message);
    }
  }

  function playRecording(name) {
    return reportAction(
      apiFetch(`/api/recordings/${encodeURIComponent(name)}/play`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({}),
      })
    );
  }

  function abortPlayback() {
    return reportAction(apiFetch("/api/playback/stop", { method: "POST" }));
  }

  function downloadRecording(name) {
    window.open(`/api/recordings/${encodeURIComponent(name)}/download`, "_blank");
  }

  function renameRecording(name) {
    const newName = window.prompt(`Rename "${name}" to:`, name);
    if (!newName || newName === name) return;
    return reportAction(
      apiFetch(`/api/recordings/${encodeURIComponent(name)}/rename`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ new_name: newName }),
      })
    );
  }

  function deleteRecording(name) {
    if (!window.confirm(`Delete "${name}"?`)) return;
    return reportAction(apiFetch(`/api/recordings/${encodeURIComponent(name)}`, { method: "DELETE" }));
  }

  els.recordForm.addEventListener("submit", (event) => {
    event.preventDefault();
    const name = els.recordName.value.trim();
    if (!name) return;
    reportAction(
      apiFetch(`/api/recordings/${encodeURIComponent(name)}/record`, { method: "POST" })
    ).then(() => {
      els.recordName.value = "";
    });
  });

  els.recordStopBtn.addEventListener("click", () => {
    reportAction(apiFetch("/api/record/stop", { method: "POST" }));
  });

  els.stopAllBtn.addEventListener("click", () => {
    reportAction(apiFetch("/api/stop", { method: "POST" }));
  });

  function connectWebSocket() {
    const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
    const socket = new WebSocket(`${protocol}//${window.location.host}/api/ws`);
    let backoffMs = 500;

    socket.addEventListener("open", () => {
      backoffMs = 500;
      els.connectionBanner.hidden = true;
    });

    socket.addEventListener("message", (event) => {
      try {
        renderStatus(JSON.parse(event.data));
      } catch (error) {
        console.error("bad status payload", error);
      }
    });

    socket.addEventListener("close", () => {
      els.connectionBanner.hidden = false;
      els.connectionBanner.textContent = "Disconnected from Zero -- retrying...";
      setTimeout(connectWebSocket, backoffMs);
      backoffMs = Math.min(backoffMs * 2, 10_000);
    });

    socket.addEventListener("error", () => socket.close());
  }

  loadRecordings();
  connectWebSocket();
})();
