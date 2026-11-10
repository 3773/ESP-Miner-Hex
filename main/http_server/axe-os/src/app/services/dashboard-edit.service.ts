import { Injectable } from '@angular/core';
import { BehaviorSubject, Subject } from 'rxjs';

export interface WidgetDef {
  id: string;
  label: string;
  x: number;
  y: number;
  w: number;
  h: number;
  minW?: number;
  minH?: number;
}

@Injectable({ providedIn: 'root' })
export class DashboardEditService {
  isActive$ = new BehaviorSubject<boolean>(false);

  editMode$ = new BehaviorSubject<boolean>(false);

  resetRequested$ = new Subject<void>();

  toggleWidgetRequested$ = new Subject<string>();

  widgetDefs: WidgetDef[] = [];

  hiddenWidgets = new Set<string>();

  // ESP-Miner-Hex: round gauges instead of bars (saved in the miner as dashboardGauges)
  gaugeView$ = new BehaviorSubject<boolean>(false);

  private gaugeViewLoaded = false;

  // take the saved view from the first system info only, so a toggle is not
  // undone by a live update that was sent before the change was saved
  syncGaugeView(value: unknown): void {
    if (this.gaugeViewLoaded || value === undefined || value === null) {
      return;
    }
    this.gaugeViewLoaded = true;
    this.gaugeView$.next(Number(value) === 1);
  }

  toggleEditMode(): void {
    this.editMode$.next(!this.editMode$.value);
  }

  requestReset(): void {
    this.resetRequested$.next();
  }

  isWidgetVisible(id: string): boolean {
    return !this.hiddenWidgets.has(id);
  }
}
