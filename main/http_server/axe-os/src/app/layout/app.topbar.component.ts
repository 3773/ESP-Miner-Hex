import { Component, ElementRef, Input, ViewChild, OnInit, OnDestroy } from '@angular/core';
import { Observable, Subject, takeUntil } from 'rxjs';
import { ToastrService } from 'ngx-toastr';
import { SystemApiService } from 'src/app/services/system.service';
import { LiveDataService } from 'src/app/services/live-data.service';
import { LayoutService } from './service/app.layout.service';
import { SensitiveData } from 'src/app/services/sensitive-data.service';
import { DashboardEditService } from 'src/app/services/dashboard-edit.service';
import { SystemInfo as ISystemInfo } from 'src/app/generated/models';

@Component({
    selector: 'app-topbar',
    templateUrl: './app.topbar.component.html',
    standalone: false
})
export class AppTopBarComponent implements OnInit, OnDestroy {
  private destroy$ = new Subject<void>();

  public info$: Observable<ISystemInfo>;
  public sensitiveDataHidden: boolean = false;
  public isMiningPaused: boolean = false;
  public isWidgetPanelOpen = false;

  @Input() isAPMode: boolean = false;

  @ViewChild('menubutton') menuButton!: ElementRef;

  constructor(
    public layoutService: LayoutService,
    private systemService: SystemApiService,
    public liveDataService: LiveDataService,
    private toastr: ToastrService,
    private sensitiveData: SensitiveData,
    public dashboardEdit: DashboardEditService,
  ) {
    this.info$ = this.liveDataService.info$;
  }

  ngOnInit() {
    this.sensitiveData.hidden
      .pipe(takeUntil(this.destroy$))
      .subscribe((hidden: boolean) => {
        this.sensitiveDataHidden = hidden;
      });

    this.info$.pipe(takeUntil(this.destroy$)).subscribe((info: ISystemInfo) => {
      if ((info as any).miningPaused !== undefined) {
        this.isMiningPaused = (info as any).miningPaused;
      }
      this.dashboardEdit.syncGaugeView((info as any).dashboardGauges);
    });
  }

  ngOnDestroy() {
    this.destroy$.next();
    this.destroy$.complete();
  }

  // ESP-Miner-Hex: dashboard refresh interval, one click = next step
  public readonly refreshSteps = [1, 2, 5, 10, 30];

  public cycleRefresh() {
    const current = this.liveDataService.refreshSeconds$.value;
    const next = this.refreshSteps.find(s => s > current) ?? this.refreshSteps[0];
    this.liveDataService.refreshSeconds$.next(next);
    this.systemService.updateSystem('', { dashboardRefresh: next })
      .pipe(takeUntil(this.destroy$))
      .subscribe({
        error: () => this.toastr.error('Could not save the refresh interval on the device'),
      });
  }

  // ESP-Miner-Hex: switch the dashboard between bars and round gauges
  public toggleGaugeView() {
    const gauges = !this.dashboardEdit.gaugeView$.value;
    this.dashboardEdit.gaugeView$.next(gauges);
    this.systemService.updateSystem('', { dashboardGauges: gauges ? 1 : 0 })
      .pipe(takeUntil(this.destroy$))
      .subscribe({
        error: () => this.toastr.error('Could not save the dashboard view on the device'),
      });
  }

  public toggleSensitiveData() {
    this.sensitiveData.toggle();
  }

  public toggleMiningPaused() {
    const action = this.isMiningPaused
      ? this.systemService.resumeMining()
      : this.systemService.pauseMining();
    const newPausedState = !this.isMiningPaused;
    action.subscribe({
      next: (response) => {
        this.isMiningPaused = newPausedState;
        this.toastr.success(response.message);
      },
      error: () => this.toastr.error('Failed to change mining state')
    });
  }

  public restart() {
    if (confirm('Are you sure you want to restart the device?')) {
      this.systemService.restart().subscribe({
        next: () => this.toastr.success('Device restarted'),
        error: () => this.toastr.error('Restart failed')
      });
    }
  }
}
