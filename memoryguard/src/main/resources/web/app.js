// Real-time chart configuration
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
            label: 'Host RAM Usage (%)',
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
                        text: 'Savings (MB)',
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
                    max: 100,
                    title: {
                        display: true,
                        text: 'Usage (%)',
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

// Update the chart with new data points
function updateChart(label, savedMb, usagePercent) {
    if (chart.data.labels.length >= maxDataPoints) {
        chart.data.labels.shift();
        chart.data.datasets[0].data.shift();
        chart.data.datasets[1].data.shift();
    }

    chart.data.labels.push(label);
    chart.data.datasets[0].data.push(savedMb);
    chart.data.datasets[1].data.push(usagePercent);
    chart.update();
}

// API interaction calls
async function fetchStatus() {
    try {
        const response = await fetch('/api/status');
        if (!response.ok) throw new Error('API request failed');
        const data = await response.json();
        
        updateUI(data);
    } catch (error) {
        console.error('Error fetching system status:', error);
    }
}

function updateUI(data) {
    // 1. Update Badge
    const badge = document.getElementById('ksm-global-badge');
    if (!data.ksmSupported) {
        badge.innerText = 'KSM: UNSUPPORTED';
        badge.className = 'status-badge inactive';
    } else if (data.ksmRunning) {
        badge.innerText = 'KSM: ACTIVE';
        badge.className = 'status-badge active';
    } else {
        badge.innerText = 'KSM: INACTIVE';
        badge.className = 'status-badge';
    }

    // 2. Update KPI cards
    document.getElementById('val-saved-mem').innerText = data.savedMemoryMb.toFixed(2) + ' MB';
    
    const scannedMb = (data.pagesToScan * 4096) / 1024 / 1024; // dummy scaling or total scanned
    // Calculate percentage based on system total memory
    const totalSystemMemoryMb = data.systemTotalMemory / 1024 / 1024;
    const percentOfTotal = totalSystemMemoryMb > 0 ? (data.savedMemoryMb / totalSystemMemoryMb * 100) : 0;
    document.getElementById('val-saved-percent').innerText = percentOfTotal.toFixed(2) + '% of total host memory';

    document.getElementById('val-savings-usd').innerText = '$' + data.monthlySavingsUsd.toFixed(2);

    document.getElementById('val-sys-usage').innerText = data.systemMemoryUsagePercent.toFixed(1) + '%';
    const totalGb = data.systemTotalMemory / 1024 / 1024 / 1024;
    const availGb = data.systemAvailableMemory / 1024 / 1024 / 1024;
    const usedGb = totalGb - availGb;
    document.getElementById('val-sys-details').innerText = usedGb.toFixed(2) + ' GB / ' + totalGb.toFixed(2) + ' GB';

    // 3. Update Inputs & Toggles (if user is not editing them)
    if (!document.activeElement || document.activeElement.id !== 'btn-toggle-ksm') {
        document.getElementById('btn-toggle-ksm').checked = data.ksmRunning;
    }
    if (!document.activeElement || document.activeElement.id !== 'btn-toggle-autotune') {
        document.getElementById('btn-toggle-autotune').checked = data.autoTune;
    }

    const manualFields = document.getElementById('manual-tuning-fields');
    if (data.autoTune) {
        manualFields.classList.add('disabled-tuning');
    } else {
        manualFields.classList.remove('disabled-tuning');
    }

    if (document.activeElement.id !== 'input-pages-to-scan') {
        document.getElementById('input-pages-to-scan').value = data.pagesToScan;
    }
    if (document.activeElement.id !== 'input-sleep-ms') {
        document.getElementById('input-sleep-ms').value = data.sleepMillisecs;
    }
    if (document.activeElement.id !== 'input-zero-pages') {
        document.getElementById('input-zero-pages').checked = data.useZeroPages;
    }

    // 4. Update Chart
    const now = new Date();
    const timeStr = now.getHours().toString().padStart(2, '0') + ':' + 
                    now.getMinutes().toString().padStart(2, '0') + ':' + 
                    now.getSeconds().toString().padStart(2, '0');
    updateChart(timeStr, data.savedMemoryMb, data.systemMemoryUsagePercent);

    // 5. Update Container Table
    const tableBody = document.getElementById('containers-table-body');
    tableBody.innerHTML = '';
    
    if (data.containers && data.containers.length > 0) {
        data.containers.forEach(c => {
            const row = document.createElement('tr');
            const ramMb = c.memoryUsageBytes / 1024 / 1024;
            
            row.innerHTML = `
                <td><strong>${c.name}</strong></td>
                <td><code>${c.image}</code></td>
                <td>${ramMb.toFixed(1)} MB</td>
                <td><small style="color: #8b949e;">.../${c.id.substring(0, 12)}/memory.current</small></td>
            `;
            tableBody.appendChild(row);
        });
    } else {
        tableBody.innerHTML = `
            <tr>
                <td colspan="4" class="text-center" style="color: #8b949e;">
                    No running containers detected on the host socket.
                </td>
            </tr>
        `;
    }
}

// Set up UI listeners
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

document.getElementById('btn-toggle-autotune').addEventListener('change', async (e) => {
    try {
        await fetch('/api/ksm/tune', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ autoTune: e.target.checked })
        });
        fetchStatus();
    } catch (err) {
        console.error('Failed to toggle Autotune:', err);
    }
});

document.getElementById('btn-apply-tune').addEventListener('click', async () => {
    const pagesToScan = parseInt(document.getElementById('input-pages-to-scan').value);
    const sleepMillisecs = parseInt(document.getElementById('input-sleep-ms').value);
    const useZeroPages = document.getElementById('input-zero-pages').checked;

    try {
        await fetch('/api/ksm/tune', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                pagesToScan: pagesToScan,
                sleepMillisecs: sleepMillisecs,
                useZeroPages: useZeroPages
            })
        });
        alert('Tuning parameters updated successfully!');
        fetchStatus();
    } catch (err) {
        console.error('Failed to update tuning:', err);
    }
});

// Bootstrap
window.addEventListener('load', () => {
    initChart();
    fetchStatus();
    // Poll metrics every 5 seconds
    setInterval(fetchStatus, 5000);
});
