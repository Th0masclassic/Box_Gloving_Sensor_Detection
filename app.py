import math
from dataclasses import dataclass

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.animation import FuncAnimation
from matplotlib.lines import Line2D
from mpl_toolkits.mplot3d import Axes3D  # noqa: F401


FSR_TRIGGER_THRESHOLD = 120.0
PRE_TRIGGER_SAMPLES = 20
POST_TRIGGER_SAMPLES = 45


@dataclass
class SensorSample:
    t: float
    accel: np.ndarray
    gyro: np.ndarray
    fsr: float
    position: np.ndarray
    punch_id: int
    transmitted: bool


class BoxingGloveTriggeredMockApp:
    def __init__(self, sample_rate_hz: int = 100, duration_s: float = 10.0):
        self.sample_rate_hz = sample_rate_hz
        self.dt = 1.0 / sample_rate_hz
        self.duration_s = duration_s

        self.samples = self._build_mock_dataset()

        self.positions = np.array([s.position for s in self.samples])
        self.times = np.array([s.t for s in self.samples])
        self.accel = np.array([s.accel for s in self.samples])
        self.gyro = np.array([s.gyro for s in self.samples])
        self.fsr = np.array([s.fsr for s in self.samples])
        self.punch_ids = np.array([s.punch_id for s in self.samples])
        self.transmitted = np.array([s.transmitted for s in self.samples])

        self.transmitted_idx = np.where(self.transmitted)[0]
        self.unique_punches = sorted([pid for pid in set(self.punch_ids) if pid != 0])

        self.punch_colors = ["C0", "C1", "C2", "C3", "C4", "C5"]
        self.punch_lines = {}

        self.fig = plt.figure(figsize=(15, 9))
        self.ax3d = self.fig.add_subplot(221, projection="3d")
        self.ax_acc = self.fig.add_subplot(222)
        self.ax_gyro = self.fig.add_subplot(223)
        self.ax_fsr = self.fig.add_subplot(224)

        self.current_point, = self.ax3d.plot([], [], [], marker="o", linestyle="", markersize=8)

        self.ax_lines = [
            self.ax_acc.plot([], [], label="Ax")[0],
            self.ax_acc.plot([], [], label="Ay")[0],
            self.ax_acc.plot([], [], label="Az")[0],
        ]
        self.gyro_lines = [
            self.ax_gyro.plot([], [], label="Gx")[0],
            self.ax_gyro.plot([], [], label="Gy")[0],
            self.ax_gyro.plot([], [], label="Gz")[0],
        ]
        self.fsr_line = self.ax_fsr.plot([], [], label="FSR")[0]
        self.threshold_line = self.ax_fsr.axhline(
            FSR_TRIGGER_THRESHOLD, linestyle="--", linewidth=1.4, label="Trigger threshold"
        )

        self._setup_axes()
        self._setup_punch_lines()

    def _setup_axes(self):
        self.fig.suptitle("Boxing Glove Triggered Transmission Mock Data Visualizer", fontsize=15)

        transmitted_positions = self.positions[self.transmitted]
        x = transmitted_positions[:, 0]
        y = transmitted_positions[:, 1]
        z = transmitted_positions[:, 2]

        self.ax3d.set_title("3D Trajectory")
        self.ax3d.set_xlabel("X")
        self.ax3d.set_ylabel("Y")
        self.ax3d.set_zlabel("Z")
        self.ax3d.set_xlim(np.min(x) - 0.1, np.max(x) + 0.1)
        self.ax3d.set_ylim(np.min(y) - 0.1, np.max(y) + 0.1)
        self.ax3d.set_zlim(np.min(z) - 0.1, np.max(z) + 0.1)

        self.ax_acc.set_title("Accelerometer Transmitted")
        self.ax_acc.set_xlabel("Time (s)")
        self.ax_acc.set_ylabel("Acceleration (m/s²)")
        self.ax_acc.grid(True)
        self.ax_acc.legend()

        self.ax_gyro.set_title("Gyroscope Transmitted")
        self.ax_gyro.set_xlabel("Time (s)")
        self.ax_gyro.set_ylabel("Angular velocity (deg/s)")
        self.ax_gyro.grid(True)
        self.ax_gyro.legend()

        self.ax_fsr.set_title("FSR and Trigger Threshold")
        self.ax_fsr.set_xlabel("Time (s)")
        self.ax_fsr.set_ylabel("Force (a.u.)")
        self.ax_fsr.grid(True)
        self.ax_fsr.legend()

        legend_handles = []
        for i, punch_id in enumerate(self.unique_punches):
            color = self.punch_colors[i % len(self.punch_colors)]
            legend_handles.append(Line2D([0], [0], color=color, lw=2.5, label=f"Soco {punch_id}"))
        self.ax3d.legend(handles=legend_handles, loc="upper left")

        self.fig.tight_layout()

    def _setup_punch_lines(self):
        for i, punch_id in enumerate(self.unique_punches):
            color = self.punch_colors[i % len(self.punch_colors)]
            line, = self.ax3d.plot([], [], [], lw=2.5, color=color)
            self.punch_lines[punch_id] = line

    def _build_mock_dataset(self):
        total_samples = int(self.duration_s * self.sample_rate_hz)
        velocity = np.zeros(3)
        position = np.zeros(3)
        raw_samples = []

        punch_windows = [
            (1.0, 2.2, 1),
            (3.6, 4.8, 2),
            (6.2, 7.4, 3),
        ]

        for i in range(total_samples):
            t = i * self.dt

            ax, ay, az = 0.0, 0.0, 0.0
            gx, gy, gz = 0.0, 0.0, 0.0
            fsr = 0.0
            punch_id = 0

            active_window = None
            for start, end, pid in punch_windows:
                if start <= t <= end:
                    active_window = (start, end, pid)
                    punch_id = pid
                    break

            if active_window is None:
                ax = 0.12 * math.sin(2 * math.pi * 0.8 * t)
                ay = 0.06 * math.cos(2 * math.pi * 1.2 * t)
                az = 0.05 * math.sin(2 * math.pi * 0.6 * t)
                gx = 2.0 * math.sin(2 * math.pi * 1.1 * t)
                gy = 1.5 * math.cos(2 * math.pi * 0.9 * t)
                gz = 1.0 * math.sin(2 * math.pi * 0.7 * t)
            else:
                start, end, pid = active_window
                p = (t - start) / (end - start)

                if p < 0.25:
                    q = p / 0.25
                    ax = 3.0 + 5.0 * q
                    ay = 0.35 * math.sin(math.pi * q)
                    az = 0.20 * math.sin(math.pi * q)
                    gx = 20.0 * q
                    gy = 10.0 * q
                    gz = 50.0 * q

                elif p < 0.60:
                    q = (p - 0.25) / 0.35
                    ax = 9.0 + 8.0 * math.sin(math.pi * q)
                    ay = 0.9 * math.sin(math.pi * q)
                    az = 0.45 * math.sin(math.pi * q)
                    gx = 35.0 + 10.0 * math.sin(math.pi * q)
                    gy = 15.0 + 6.0 * math.sin(math.pi * q)
                    gz = 120.0

                elif p < 0.72:
                    q = (p - 0.60) / 0.12
                    ax = -10.0 - 6.0 * math.sin(math.pi * q)
                    ay = 0.15 * math.sin(math.pi * q)
                    az = -0.25 * math.sin(math.pi * q)
                    gx = -30.0
                    gy = -12.0
                    gz = -80.0
                    fsr = 900.0 * math.exp(-((q - 0.5) ** 2) / 0.025)

                else:
                    q = (p - 0.72) / 0.28
                    ax = -6.0 * math.sin(math.pi * q)
                    ay = -0.7 * math.sin(math.pi * q)
                    az = -0.2 * math.sin(math.pi * q)
                    gx = -18.0 * math.sin(math.pi * q)
                    gy = -8.0 * math.sin(math.pi * q)
                    gz = -55.0 * math.sin(math.pi * q)

                ax += 0.6 * (pid - 2)
                gz += 10.0 * (pid - 2)

            accel = np.array([
                ax + np.random.normal(0, 0.08),
                ay + np.random.normal(0, 0.04),
                az + np.random.normal(0, 0.04),
            ])
            gyro = np.array([
                gx + np.random.normal(0, 1.2),
                gy + np.random.normal(0, 1.2),
                gz + np.random.normal(0, 1.8),
            ])
            fsr = max(0.0, fsr + np.random.normal(0, 6.0))

            accel_for_motion = accel.copy()
            accel_for_motion[0] *= 0.42
            accel_for_motion[1] *= 0.26
            accel_for_motion[2] *= 0.18

            velocity += accel_for_motion * self.dt
            velocity *= 0.965
            position += velocity * self.dt

            if punch_id == 1:
                position[1] += 0.0005
            elif punch_id == 2:
                position[1] -= 0.0004
            elif punch_id == 3:
                position[2] += 0.0002

            raw_samples.append(
                {
                    "t": t,
                    "accel": accel,
                    "gyro": gyro,
                    "fsr": fsr,
                    "position": position.copy(),
                    "punch_id": punch_id,
                }
            )

        transmitted_flags = np.zeros(len(raw_samples), dtype=bool)

        trigger_indices = []
        last_trigger_idx = -POST_TRIGGER_SAMPLES - 1
        for i, s in enumerate(raw_samples):
            if s["fsr"] >= FSR_TRIGGER_THRESHOLD and (i - last_trigger_idx) > POST_TRIGGER_SAMPLES:
                trigger_indices.append(i)
                last_trigger_idx = i

        for trig_idx in trigger_indices:
            start_idx = max(0, trig_idx - PRE_TRIGGER_SAMPLES)
            end_idx = min(len(raw_samples), trig_idx + POST_TRIGGER_SAMPLES + 1)
            transmitted_flags[start_idx:end_idx] = True

        samples = []
        for i, s in enumerate(raw_samples):
            samples.append(
                SensorSample(
                    t=s["t"],
                    accel=s["accel"],
                    gyro=s["gyro"],
                    fsr=s["fsr"],
                    position=s["position"],
                    punch_id=s["punch_id"],
                    transmitted=bool(transmitted_flags[i]),
                )
            )

        return samples

    def _update(self, frame):
        upto = frame + 1

        visible_mask = self.transmitted[:upto]
        visible_positions = self.positions[:upto][visible_mask]
        visible_times = self.times[:upto][visible_mask]
        visible_accel = self.accel[:upto][visible_mask]
        visible_gyro = self.gyro[:upto][visible_mask]
        visible_fsr = self.fsr[:upto]
        visible_times_fsr = self.times[:upto]

        for punch_id in self.unique_punches:
            mask = (self.punch_ids[:upto] == punch_id) & self.transmitted[:upto]
            pos = self.positions[:upto][mask]
            if len(pos) > 0:
                self.punch_lines[punch_id].set_data(pos[:, 0], pos[:, 1])
                self.punch_lines[punch_id].set_3d_properties(pos[:, 2])

        if len(visible_positions) > 0:
            current_pos = visible_positions[-1]
            current_punch_candidates = self.punch_ids[:upto][visible_mask]
            current_punch = current_punch_candidates[-1] if len(current_punch_candidates) else 0
            current_color = "k"
            if current_punch in self.unique_punches:
                idx = self.unique_punches.index(current_punch)
                current_color = self.punch_colors[idx % len(self.punch_colors)]
            self.current_point.set_data([current_pos[0]], [current_pos[1]])
            self.current_point.set_3d_properties([current_pos[2]])
            self.current_point.set_color(current_color)

        if len(visible_times) > 0:
            self.ax_lines[0].set_data(visible_times, visible_accel[:, 0])
            self.ax_lines[1].set_data(visible_times, visible_accel[:, 1])
            self.ax_lines[2].set_data(visible_times, visible_accel[:, 2])

            self.gyro_lines[0].set_data(visible_times, visible_gyro[:, 0])
            self.gyro_lines[1].set_data(visible_times, visible_gyro[:, 1])
            self.gyro_lines[2].set_data(visible_times, visible_gyro[:, 2])

        self.fsr_line.set_data(visible_times_fsr, visible_fsr)

        self.ax_acc.set_xlim(0, self.duration_s)
        self.ax_gyro.set_xlim(0, self.duration_s)
        self.ax_fsr.set_xlim(0, self.duration_s)

        self.ax_acc.relim()
        self.ax_acc.autoscale_view(scalex=False, scaley=True)

        self.ax_gyro.relim()
        self.ax_gyro.autoscale_view(scalex=False, scaley=True)

        self.ax_fsr.relim()
        self.ax_fsr.autoscale_view(scalex=False, scaley=True)

        artists = list(self.punch_lines.values())
        artists.extend([
            self.current_point,
            *self.ax_lines,
            *self.gyro_lines,
            self.fsr_line,
            self.threshold_line,
        ])
        return artists

    def run(self):
        self.anim = FuncAnimation(
            self.fig,
            self._update,
            frames=len(self.samples),
            interval=int(self.dt * 1000),
            blit=False,
            repeat=True,
        )
        plt.show()


if __name__ == "__main__":
    app = BoxingGloveTriggeredMockApp(sample_rate_hz=100, duration_s=10.0)
    app.run()
