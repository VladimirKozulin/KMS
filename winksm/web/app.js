// WinKSM Dashboard JavaScript
// Polls the WinKSM service API and updates the dashboard in real-time.

let chart;
const maxDataPoints = 30;
const chartData = {
    labels: [],
    datasets: [
        {
            label: 'Saved Memory (MB)',
            borderColor: '#238636',
            backgroundColor: 'rgba(35, 134, 84, 0.1)',
            borderWidth: 2,
            fill: true,
            data: [],
            yAxisID: 'y'
        },
        {
            label: 'Pages Shared',
            borderColor: '#58a6ff',
            backgroundColor: 'transparent',
            borderWidth: 2,
            borderDash: [5, 5],
            data: [],
            yAxisID: 'y1'
        }
    ]
};

function initChart() {
    const ctx = document.getElementById('savingsChart').getContext('2d');
    chart = new Chart(ctx, {
        type: 'line',
        data: chartData,
        options: {
            responsive: true,
            maintainAspectRatio: false,
            interaction: {
                mode: 'index',
                intersect: false,
            },
            scales: {
                x: {
                    grid: { color: '#21262d' },
                    ticks: { color: '#8b949e' }
                },
                y: {
                    type: 'linear',
                    display: true,
                    position: 'left',
                    grid: { color: '#21262d' },
                    ticks: { color: '#8b949e' },
                    title: {
                        display: true,
                        text: 'Saved (MB)',
                        color: '#8b949e'
                    }
                },
                y1: {
                    type: 'linear',
                    display: true,
                    position: 'right',
                    grid: { drawOnChartArea: false },
                    ticks: { color: '#8b949e' },
                    min: 0,
                    title: {
                        display: true,
                        text: 'Shared Pages',
                        color: '#8b949e'
                    }
                }
            },
            plugins: {
                legend: {
                    labels: { color: '#c9d1d9' }
                }
            }
        }
    });
}

function updateChart(label, savedMb, sharedPages) {
    if (chart.data.labels.length >= maxDataPoints) {
        chart.data.labels.shift();
        chart.data.datasets[0].data.shift();
        chart.data.datasets[1].data.shift();
    }

    chart.data.labels.push(label);
    chart.data.datasets[0].data.push(savedMb);
    chart.data.datasets[1].data.push(sharedPages);
    chart.update();
}

async function fetchStatus() {
    try {
        const response = await fetch('/api/status');
        if (!response.ok) throw new Error('API request failed');
        const data = await response.json();
        updateUI(data);
    } catch (error) {
        console.error('Error fetching status:', error);
        document.getElementById('ksm-global-badge').innerText = 'KSM: OFFLINE';
        document.getElementById('ksm-global-badge').className = 'status-badge inactive';
    }
}

function updateUI(data) {
    // Update badge
    const badge = document.getElementById('ksm-global-badge');
    if (data.ksmRunning) {
        badge.innerText = 'KSM: ACTIVE';
        badge.className = 'status-badge active';
    } else {
        badge.innerText = 'KSM: INACTIVE';
        badge.className = 'status-badge';
    }

    // Update KPI cards
    document.getElementById('val-saved-mem').innerText = data.savedMemoryMb.toFixed(2) + ' MB';
    document.getElementById('val-saved-pages').innerText = data.pagesSharing + ' pages shared';
    document.getElementById('val-savings-usd').innerText = '$' + data.monthlySavingsUsd.toFixed(2);

    // Scan status
    document.getElementById('val-scan-status').innerText = data.ksmRunning ? 'Scanning...' : 'Stopped';
    document.getElementById('val-scan-details').innerText = data.fullScans + ' full scans completed';

    // Toggle states
    document.getElementById('btn-toggle-ksm').checked = data.ksmRunning;

    // Update detailed stats
    document.getElementById('stat-scanned').innerText = formatNumber(data.pagesScanned);
    document.getElementById('stat-shared').innerText = formatNumber(data.pagesShared);
    document.getElementById('stat-sharing').innerText = formatNumber(data.pagesSharing);
    document.getElementById('stat-unshared').innerText = formatNumber(data.pagesUnshared);
    document.getElementById('stat-volatile').innerText = formatNumber(data.pagesVolatile);
    document.getElementById('stat-zero').innerText = formatNumber(data.zeroPagesShared);
    document.getElementById('stat-fullscans').innerText = formatNumber(data.fullScans);
    document.getElementById('stat-processes').innerText = data.processCount;

    // Update chart
    const now = new Date();
    const timeStr = now.getHours().toString().padStart(2, '0') + ':' +
                    now.getMinutes().toString().padStart(2, '0') + ':' +
                    now.getSeconds().toString().padStart(2, '0');
    updateChart(timeStr, data.savedMemoryMb, data.pagesShared);
}

function formatNumber(num) {
    if (num >= 1000000) {
        return (num / 1000000).toFixed(1) + 'M';
    } else if (num >= 1000) {
        return (num / 1000).toFixed(1) + 'K';
    }
    return num.toString();
}

// API interaction
document.getElementById('btn-toggle-ksm').addEventListener('change', async (e) => {
    try {
        await fetch('/api/ksm/toggle', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ run: e.target.checked })
        });
        fetchStatus();
    } catch (err) {
        console.error('Failed to toggle KSM:', err);
    }
});

document.getElementById('btn-apply-tune').addEventListener('click', async () => {
    const pagesToScan = parseInt(document.getElementById('input-pages-to-scan').value);
    const sleepMs = parseInt(document.getElementById('input-sleep-ms').value);
    const maxSharing = parseInt(document.getElementById('input-max-sharing').value);
    const smartScan = document.getElementById('btn-toggle-smart').checked;
    const useZero = document.getElementById('btn-toggle-zero').checked;

    try {
        await fetch('/api/ksm/tune', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                pagesToScan: pagesToScan,
                sleepMillisecs: sleepMs,
                maxPageSharing: maxSharing,
                smartScan: smartScan,
                useZeroPages: useZero
            })
        });
        fetchStatus();
    } catch (err) {
        console.error('Failed to update tuning:', err);
    }
});

// Bootstrap
window.addEventListener('load', () => {
    initChart();
    fetchStatus();
    // Poll metrics every 3 seconds
    setInterval(fetchStatus, 3000);
});
