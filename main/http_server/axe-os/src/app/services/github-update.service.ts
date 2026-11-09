import { HttpClient } from '@angular/common/http';
import { Injectable } from '@angular/core';
import { Observable } from 'rxjs';
import { map } from 'rxjs/operators';


interface GithubRelease {
  id: number;
  tag_name: string;
  name: string;
  prerelease: boolean;
}

@Injectable({
  providedIn: 'root'
})
export class GithubUpdateService {

  constructor(
    private httpClient: HttpClient
  ) { }


  public getReleases(): Observable<GithubRelease[]> {
    return this.httpClient.get<GithubRelease[]>(
      // ESP-Miner-Hex: releases of Daniel's fork (bitaxeHex fix), not of bitaxeorg
      'https://api.github.com/repos/3773/ESP-Miner-Hex/releases'
    ).pipe(
      map((releases: GithubRelease[]) => releases.filter((release: GithubRelease) => !release.prerelease))
    );
  }

}
