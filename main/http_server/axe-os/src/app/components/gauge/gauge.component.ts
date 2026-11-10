import { Component, Input } from '@angular/core';
import { CommonModule } from '@angular/common';

// ESP-Miner-Hex: round gauge in the style of the old AxeOS knobs. A 300 degree
// arc open at the bottom, the value in the middle, the label underneath.
// Uses the same theme colours as app-progressbar.
@Component({
  selector: 'app-gauge',
  standalone: true,
  imports: [CommonModule],
  template: `
    <div class="gauge" [class.gauge-danger]="danger">
      <svg viewBox="0 0 100 100" role="img" [attr.aria-label]="label + ': ' + text">
        <path class="gauge-track" [attr.d]="trackPath" />
        @if (fraction > 0) {
          <path class="gauge-value" [attr.d]="valuePath" />
        }
        @if (markerPoint) {
          <line class="gauge-marker" [attr.x1]="markerPoint.x1" [attr.y1]="markerPoint.y1"
                [attr.x2]="markerPoint.x2" [attr.y2]="markerPoint.y2" />
        }
        <text class="gauge-text" x="50" [attr.y]="sub ? 50 : 55" text-anchor="middle">{{ text }}</text>
        @if (sub) {
          <text class="gauge-sub" x="50" y="66" text-anchor="middle">{{ sub }}</text>
        }
      </svg>
      <div class="gauge-label">{{ label }}</div>
    </div>
  `,
  styleUrl: './gauge.component.scss'
})
export class GaugeComponent {
  @Input() value: number | null | undefined = 0;
  @Input() min: number = 0;
  @Input() max: number = 100;
  @Input() unit: string = '';
  @Input() decimals: number = 0;
  @Input() label: string = '';
  @Input() sub: string = '';            // small second line inside the circle
  @Input() danger: boolean = false;     // red arc and text
  @Input() marker: number | null = null; // tick on the arc (e.g. target temperature)
  @Input() empty: boolean = false;      // show "--" instead of the value

  private static readonly R = 40;
  private static readonly START = 240; // degrees, measured counter-clockwise from 3 o'clock
  private static readonly SWEEP = 300;

  get fraction(): number {
    const v = Number(this.value);
    if (this.empty || !isFinite(v) || this.max <= this.min) {
      return 0;
    }
    return Math.max(0, Math.min(1, (v - this.min) / (this.max - this.min)));
  }

  get text(): string {
    const v = Number(this.value);
    if (this.empty || !isFinite(v)) {
      return '--';
    }
    return v.toFixed(this.decimals) + (this.unit ? ' ' + this.unit : '');
  }

  get trackPath(): string {
    return this.arc(0, 1);
  }

  get valuePath(): string {
    return this.arc(0, this.fraction);
  }

  get markerPoint(): { x1: number; y1: number; x2: number; y2: number } | null {
    if (this.marker === null || this.marker === undefined || !isFinite(Number(this.marker)) || this.max <= this.min) {
      return null;
    }
    const f = Math.max(0, Math.min(1, (Number(this.marker) - this.min) / (this.max - this.min)));
    const a = this.angle(f);
    const inner = GaugeComponent.R - 9;
    const outer = GaugeComponent.R + 9;
    return {
      x1: 50 + inner * Math.cos(a), y1: 50 - inner * Math.sin(a),
      x2: 50 + outer * Math.cos(a), y2: 50 - outer * Math.sin(a),
    };
  }

  private angle(f: number): number {
    return (GaugeComponent.START - GaugeComponent.SWEEP * f) * Math.PI / 180;
  }

  private arc(from: number, to: number): string {
    const r = GaugeComponent.R;
    const a0 = this.angle(from);
    const a1 = this.angle(Math.max(to, from + 0.0001));
    const x0 = 50 + r * Math.cos(a0), y0 = 50 - r * Math.sin(a0);
    const x1 = 50 + r * Math.cos(a1), y1 = 50 - r * Math.sin(a1);
    const large = (to - from) * GaugeComponent.SWEEP > 180 ? 1 : 0;
    return `M ${x0.toFixed(2)} ${y0.toFixed(2)} A ${r} ${r} 0 ${large} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
  }
}
