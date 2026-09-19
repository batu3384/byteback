import React, { useState, useEffect } from 'react';
import './RaidBuilder.css';
import { Layers, HardDrive, Cpu, Settings2, Play, Radar } from 'lucide-react';
import InlineAlert from '../InlineAlert';
import { useI18n, tFormat } from '../../i18n';
import {
  RAID_OFFSET_SECTORS_MAX,
  RAID_STRIPE_BYTES,
  formatRaidStripe,
  isRaidStripeSize,
  raid5StripeAmbiguous,
  raidAssembleRequest,
  applyRaidMemberOrder,
  RAID5_ALGO_LEFT_ASYMMETRIC,
  RAID5_ALGO_LEFT_SYMMETRIC,
  RAID5_ALGO_RIGHT_ASYMMETRIC,
  RAID5_ALGO_RIGHT_SYMMETRIC,
} from '../../../shared/raid-geometry';

interface Disk {
  id: string;
  name: string;
  capacity: string;
}

interface RaidBuilderProps {
  onStartRaidScan?: (scanType: string) => void
}

// Numeric RaidLevel enum (virtual_raid.h) <-> dropdown labels.
const RAID_LEVEL_TO_LABEL: Record<number, string> = {
  0: 'RAID 0', 1: 'RAID 1', 2: 'RAID 5', 3: 'RAID 6', 4: 'RAID 10', 5: 'JBOD', 6: 'RAID 1E',
};
const RAID_LABEL_TO_LEVEL: Record<string, number> = {
  'RAID 0': 0, 'RAID 1': 1, 'RAID 5': 2, 'RAID 6': 3, 'RAID 10': 4, 'JBOD': 5, 'RAID 1E': 6,
};

const RaidBuilder: React.FC<RaidBuilderProps> = ({ onStartRaidScan }) => {
  const { t } = useI18n()
  const [availableDisks, setAvailableDisks] = useState<Disk[]>([]);
  const [raidArray, setRaidArray] = useState<Disk[]>([]);
  const [raidType, setRaidType] = useState<string>('RAID 5');
  const [isBuilding, setIsBuilding] = useState(false);
  const [isDetecting, setIsDetecting] = useState(false);
  const [assembled, setAssembled] = useState(false);
  const [failedSlots, setFailedSlots] = useState<Set<number>>(new Set());
  const [raidNotice, setRaidNotice] = useState<{ variant: 'success' | 'error' | 'warning'; message: string } | null>(null);
  const [stripeBytes, setStripeBytes] = useState(65536);
  const [offsetSectors, setOffsetSectors] = useState(0);
  const [raid5Algorithm, setRaid5Algorithm] = useState(RAID5_ALGO_LEFT_ASYMMETRIC);

  useEffect(() => {
    if (window.api && window.api.listDrives) {
      let alive = true
      window.api.listDrives().then((drives: any[]) => {
        if (!alive) return
        const disks = drives.map(d => ({
          id: d.index.toString(),
          name: tFormat('raid.diskName', { n: String(d.index), model: d.model }),
          capacity: `${(d.sizeBytes / (1024 ** 3)).toFixed(2)} GB`
        }));
        setAvailableDisks(disks);
      }).catch((e: unknown) => {
        console.error(e);
        if (alive) setRaidNotice({ variant: 'error', message: t('raid.drivesFailed') });
      });
      return () => { alive = false }
    }
  }, []);

  const handleDragStart = (e: React.DragEvent, diskId: string, from: 'available' | 'array') => {
    e.dataTransfer.setData('diskId', diskId);
    e.dataTransfer.setData('from', from);
  };

  const moveDisk = (diskId: string, from: 'available' | 'array', to: 'available' | 'array') => {
    if (from === to) return
    if (from === 'available' && to === 'array') {
      const disk = availableDisks.find(d => d.id === diskId)
      if (disk) {
        setAvailableDisks(availableDisks.filter(d => d.id !== diskId))
        setRaidArray([...raidArray, disk])
      }
    } else if (from === 'array' && to === 'available') {
      const disk = raidArray.find(d => d.id === diskId)
      if (disk) {
        setRaidArray(raidArray.filter(d => d.id !== diskId))
        setAvailableDisks([...availableDisks, disk])
      }
    }
  }

  const handleDrop = (e: React.DragEvent, to: 'available' | 'array') => {
    e.preventDefault()
    const diskId = e.dataTransfer.getData('diskId')
    const from = e.dataTransfer.getData('from') as 'available' | 'array'
    moveDisk(diskId, from, to)
  }

  const detectRaid = async () => {
    if (raidArray.length < 2 || isDetecting) return;
    const detectReq = raidAssembleRequest(raidArray.map(d => d.id));
    if (!detectReq) {
      setRaidNotice({ variant: 'warning', message: t('raid.mixMembers') });
      return;
    }
    setIsDetecting(true);
    setRaidNotice(null);
    if (!(window.api && window.api.detectRaid)) {
      setIsDetecting(false);
      setRaidNotice({ variant: 'error', message: t('raid.noBackend') });
      return;
    }
    try {
      const res = detectReq.kind === 'drive'
        ? await window.api.detectRaid(detectReq.indices)
        : await window.api.detectRaidImages(detectReq.paths);
      if (res && res.found && typeof res.raidLevel === 'number') {
        const label = RAID_LEVEL_TO_LABEL[res.raidLevel] ?? String(res.raidLevel);
        setRaidType(label);
        if (isRaidStripeSize(res.blockSize)) setStripeBytes(res.blockSize);
        if (typeof res.dataOffsetSectors === 'number' && Number.isInteger(res.dataOffsetSectors) && res.dataOffsetSectors >= 0) {
          setOffsetSectors(Math.min(res.dataOffsetSectors, RAID_OFFSET_SECTORS_MAX));
        }
        const ordered = applyRaidMemberOrder(raidArray, res.memberOrder);
        const reordered = !!(ordered && ordered.some((d, i) => d.id !== raidArray[i].id));
        if (ordered) setRaidArray(ordered);
        if (typeof res.raid5Algorithm === 'number' && res.raid5Algorithm >= 0 && res.raid5Algorithm <= 3) {
          setRaid5Algorithm(res.raid5Algorithm);
        }
        const confPct = res.confidence != null ? Math.round(res.confidence * 100) : null;
        const stripeLabel = isRaidStripeSize(res.blockSize) ? formatRaidStripe(res.blockSize) : null;
        const details = [
          confPct != null ? tFormat('raid.detectConfidence', { n: String(confPct) }) : '',
          stripeLabel != null ? tFormat('raid.detectStripe', { n: stripeLabel }) : '',
          res.dataOffsetSectors != null ? tFormat('raid.detectOffset', { n: String(res.dataOffsetSectors) }) : '',
          reordered ? t('raid.detectReordered') : '',
          res.raidLevel === 2 && res.raid5Algorithm === RAID5_ALGO_LEFT_SYMMETRIC ? t('raid.detectLeftSym') : '',
          res.raidLevel === 2 && res.raid5Algorithm === RAID5_ALGO_RIGHT_ASYMMETRIC ? t('raid.detectRightAsym') : '',
          res.raidLevel === 2 && res.raid5Algorithm === RAID5_ALGO_RIGHT_SYMMETRIC ? t('raid.detectRightSym') : '',
        ].filter(Boolean).join(' · ');
        const ambiguous = raid5StripeAmbiguous(res.raidLevel, res.confidence, res.fsConfirmed);
        setRaidNotice({
          variant: ambiguous ? 'warning' : 'success',
          message: (details
            ? tFormat('raid.detectOk', { type: label }) + ' — ' + details
            : tFormat('raid.detectOk', { type: label }))
            + (ambiguous ? ' ' + t('raid.stripeAmbiguous') : ''),
        });
      } else {
        // Honest fail: RAID 0 carries no parity and is never guessed.
        setRaidNotice({ variant: 'warning', message: t('raid.detectFailed') });
      }
    } catch (e: unknown) {
      const msg = e instanceof Error ? e.message : String(e);
      setRaidNotice({
        variant: 'error',
        message: t('raid.detectFailed') + tFormat('raid.errorSuffix', { err: msg }),
      });
    } finally {
      setIsDetecting(false);
    }
  };

  const addMemberImages = async () => {
    if (!window.api?.pickRaidMemberImages) return
    const picked = await window.api.pickRaidMemberImages()
    const existing = new Set([...availableDisks, ...raidArray].map(d => d.id))
    const added = picked.filter(p => !existing.has(p)).map(p => ({
      id: p,
      name: p.replace(/^.*[/\\]/, ''),
      capacity: t('raid.imageMember'),
    }))
    if (added.length > 0) setAvailableDisks([...availableDisks, ...added])
  }

  const buildRaid = async () => {
    if (raidArray.length < 2) return;
    setIsBuilding(true);
    // Pass the drives in their user-ordered slot order, since stripe/parity
    // layout depends on it.
    const raidLevel = RAID_LABEL_TO_LEVEL[raidType] ?? 2;
    const assembledReq = raidAssembleRequest(raidArray.map(d => d.id));
    if (!assembledReq) {
      setIsBuilding(false);
      setRaidNotice({ variant: 'error', message: t('raid.mixMembers') });
      return;
    }
    if (!(window.api && window.api.reconstructRaid)) {
      setIsBuilding(false);
      setRaidNotice({
        variant: 'error',
        message: t('raid.noBackend'),
      });
      return;
    }
    try {
      const res = assembledReq.kind === 'drive'
        ? await window.api.reconstructRaid(assembledReq.indices, raidLevel, stripeBytes, offsetSectors, raid5Algorithm)
        : await window.api.reconstructRaidImages(assembledReq.paths, raidLevel, stripeBytes, offsetSectors, raid5Algorithm);
      if (res && res.success) {
        setAssembled(true);
        setFailedSlots(new Set());
        const capGb = res.capacity ? (res.capacity / (1024 ** 3)).toFixed(2) : '?';
        setRaidNotice({
          variant: 'success',
          message: tFormat('raid.builtOk', { type: raidType, cap: capGb, n: String(res.numDisks) }),
        });
        if (onStartRaidScan) onStartRaidScan('quick');
      } else {
        setAssembled(false);
        const why = res && res.error ? tFormat('raid.errorSuffix', { err: res.error }) : '';
        setRaidNotice({
          variant: 'error',
          message: tFormat('raid.buildFailed', { type: raidType }) + why,
        });
      }
    } catch (e: unknown) {
      setAssembled(false);
      const msg = e instanceof Error ? e.message : String(e);
      setRaidNotice({
        variant: 'error',
        message: tFormat('raid.buildFailed', { type: raidType }) + tFormat('raid.errorSuffix', { err: msg }),
      });
    } finally {
      setIsBuilding(false);
    }
  };

  const assembleLvm = async () => {
    if (raidArray.length < 2 || isBuilding) return;
    const assembledReq = raidAssembleRequest(raidArray.map(d => d.id));
    if (!assembledReq) {
      setRaidNotice({ variant: 'error', message: t('raid.mixMembers') });
      return;
    }
    if (!window.api) {
      setRaidNotice({ variant: 'error', message: t('raid.noBackend') });
      return;
    }
    setIsBuilding(true);
    setRaidNotice(null);
    try {
      const res = assembledReq.kind === 'drive'
        ? await window.api.assembleLvm(assembledReq.indices)
        : await window.api.assembleLvmImages(assembledReq.paths);
      if (res && res.success) {
        setAssembled(true);
        setFailedSlots(new Set());
        const capGb = res.capacity ? (res.capacity / (1024 ** 3)).toFixed(2) : '?';
        setRaidNotice({
          variant: 'success',
          message: tFormat('raid.lvmOk', { cap: capGb, n: String(res.numDisks) }),
        });
        if (onStartRaidScan) onStartRaidScan('quick');
      } else {
        setAssembled(false);
        const why = res && res.error ? tFormat('raid.errorSuffix', { err: res.error }) : '';
        setRaidNotice({
          variant: 'error',
          message: t('raid.lvmFailed') + why,
        });
      }
    } catch (e: unknown) {
      setAssembled(false);
      const msg = e instanceof Error ? e.message : String(e);
      setRaidNotice({
        variant: 'error',
        message: t('raid.lvmFailed') + tFormat('raid.errorSuffix', { err: msg }),
      });
    } finally {
      setIsBuilding(false);
    }
  };

  const assembleLdm = async () => {
    if (raidArray.length < 2 || isBuilding) return;
    const assembledReq = raidAssembleRequest(raidArray.map(d => d.id));
    if (!assembledReq) {
      setRaidNotice({ variant: 'error', message: t('raid.mixMembers') });
      return;
    }
    if (!window.api) {
      setRaidNotice({ variant: 'error', message: t('raid.noBackend') });
      return;
    }
    setIsBuilding(true);
    setRaidNotice(null);
    try {
      const res = assembledReq.kind === 'drive'
        ? await window.api.assembleLdm(assembledReq.indices)
        : await window.api.assembleLdmImages(assembledReq.paths);
      if (res && res.success) {
        setAssembled(true);
        setFailedSlots(new Set());
        const capGb = res.capacity ? (res.capacity / (1024 ** 3)).toFixed(2) : '?';
        setRaidNotice({
          variant: 'success',
          message: tFormat('raid.ldmOk', { cap: capGb, n: String(res.numDisks) }),
        });
        if (onStartRaidScan) onStartRaidScan('quick');
      } else {
        setAssembled(false);
        const why = res && res.error ? tFormat('raid.errorSuffix', { err: res.error }) : '';
        setRaidNotice({
          variant: 'error',
          message: t('raid.ldmFailed') + why,
        });
      }
    } catch (e: unknown) {
      setAssembled(false);
      const msg = e instanceof Error ? e.message : String(e);
      setRaidNotice({
        variant: 'error',
        message: t('raid.ldmFailed') + tFormat('raid.errorSuffix', { err: msg }),
      });
    } finally {
      setIsBuilding(false);
    }
  };

  return (
    <div className="raid-builder-container">
      <div className="raid-header glass-panel">
        <div className="examiner-icon" aria-hidden="true">
          <Layers size={32} color="var(--accent-blue)" />
        </div>
        <div>
          <h2>{t('raid.title')}</h2>
          <p>{t('raid.subtitle')}</p>
        </div>
      </div>

      {raidNotice && (
        <InlineAlert variant={raidNotice.variant} onDismiss={() => setRaidNotice(null)}>
          {raidNotice.message}
        </InlineAlert>
      )}

      <div className="raid-workspace">
        <div
          className="raid-column glass-panel available-column"
          onDragOver={(e) => e.preventDefault()}
          onDrop={(e) => handleDrop(e, 'available')}
        >
          <div className="raid-col-head">
            <HardDrive size={20} color="var(--accent-blue)" aria-hidden="true" />
            <h3>{t('raid.available')}</h3>
            <button type="button" className="btn-secondary" data-testid="raid-add-images" onClick={() => { void addMemberImages() }}>
              {t('raid.addImages')}
            </button>
          </div>
          <div className="disk-list" role="list" aria-label={t('raid.available')}>
            {availableDisks.map(disk => (
              <div
                key={disk.id}
                className="disk-item"
                role="listitem"
                draggable
                onDragStart={(e) => handleDragStart(e, disk.id, 'available')}
              >
                <HardDrive size={24} color="var(--text-muted)" aria-hidden="true" />
                <div className="disk-info">
                  <div className="disk-name">{disk.name}</div>
                  <div className="disk-capacity">{disk.capacity}</div>
                </div>
                <button type="button" className="btn-secondary" onClick={() => moveDisk(disk.id, 'available', 'array')}>{t('raid.addToArray')}</button>
              </div>
            ))}
            {availableDisks.length === 0 && (
              <div className="examiner-empty" role="status">
                <div className="examiner-icon" aria-hidden="true">
                  <HardDrive size={28} />
                </div>
                <p>{t('raid.emptyAvailable')}</p>
              </div>
            )}
          </div>
        </div>

        <div
          className="raid-column glass-panel array-column"
          onDragOver={(e) => e.preventDefault()}
          onDrop={(e) => handleDrop(e, 'array')}
        >
          <div className="array-header">
            <div className="array-header-title">
              <Cpu size={20} color="var(--accent-blue)" aria-hidden="true" />
              <h3>{t('raid.arrayTitle')}</h3>
            </div>
            <div className="array-header-tools">
              <Settings2 size={16} color="var(--text-muted)" aria-hidden="true" />
              <select
                className="raid-type-select"
                value={raidType}
                onChange={(e) => setRaidType(e.target.value)}
                aria-label={t('raid.arrayTitle')}
              >
                <option value="RAID 0">{t('raid.raid0')}</option>
                <option value="RAID 1">{t('raid.raid1')}</option>
                <option value="RAID 5">{t('raid.raid5')}</option>
                <option value="RAID 6">{t('raid.raid6')}</option>
                <option value="RAID 10">{t('raid.raid10')}</option>
                <option value="JBOD">{t('raid.jbod')}</option>
                <option value="RAID 1E">{t('raid.raid1e')}</option>
              </select>
              <select
                className="raid-type-select"
                data-testid="raid-stripe-select"
                value={String(stripeBytes)}
                disabled={raidType === 'RAID 1' || raidType === 'JBOD' || assembled}
                aria-label={t('raid.stripeLabel')}
                onChange={(e) => setStripeBytes(Number(e.target.value))}
              >
                {RAID_STRIPE_BYTES.map((n) => (
                  <option key={n} value={n}>{formatRaidStripe(n)}</option>
                ))}
              </select>
              {raidType === 'RAID 5' && (
                <select
                  className="raid-type-select"
                  data-testid="raid5-algorithm-select"
                  value={String(raid5Algorithm)}
                  disabled={assembled}
                  aria-label={t('raid.algoLabel')}
                  onChange={(e) => setRaid5Algorithm(Number(e.target.value))}
                >
                  <option value={RAID5_ALGO_LEFT_ASYMMETRIC}>{t('raid.algoLeftAsym')}</option>
                  <option value={RAID5_ALGO_LEFT_SYMMETRIC}>{t('raid.algoLeftSym')}</option>
                  <option value={RAID5_ALGO_RIGHT_ASYMMETRIC}>{t('raid.algoRightAsym')}</option>
                  <option value={RAID5_ALGO_RIGHT_SYMMETRIC}>{t('raid.algoRightSym')}</option>
                </select>
              )}
              <label className="raid-offset-label">
                {t('raid.offsetLabel')}
                <input
                  data-testid="raid-offset-sectors"
                  type="number"
                  min={0}
                  max={RAID_OFFSET_SECTORS_MAX}
                  step={1}
                  value={offsetSectors}
                  disabled={assembled}
                  aria-label={t('raid.offsetLabel')}
                  onChange={(e) => {
                    const n = Number(e.target.value)
                    if (Number.isInteger(n) && n >= 0 && n <= RAID_OFFSET_SECTORS_MAX) setOffsetSectors(n)
                  }}
                />
              </label>
            </div>
          </div>

          <div className="disk-list raid-slots" role="list" aria-label={t('raid.arrayTitle')}>
            {raidArray.map((disk, index) => (
              <div
                key={disk.id}
                className="disk-item in-array"
                role="listitem"
                draggable
                onDragStart={(e) => handleDragStart(e, disk.id, 'array')}
              >
                <div className="raid-slot">{tFormat('raid.slot', { n: String(index) })}</div>
                <HardDrive size={24} color="var(--accent-blue)" aria-hidden="true" />
                <div className="disk-info">
                  <div className="disk-name">{disk.name}</div>
                  <div className="disk-capacity">{disk.capacity}</div>
                </div>
                <button type="button" className="btn-secondary" onClick={() => moveDisk(disk.id, 'array', 'available')}>{t('raid.remove')}</button>
                {assembled && (
                  <label className="fail-member">
                    <input
                      type="checkbox"
                      checked={failedSlots.has(index)}
                      disabled={failedSlots.has(index)}
                      onChange={async () => {
                        if (!window.api?.failRaidDisk) return;
                        try {
                          const ok = await window.api.failRaidDisk(index);
                          if (ok) setFailedSlots(prev => new Set(prev).add(index));
                          else setRaidNotice({ variant: 'error', message: t('raid.failMemberFailed') });
                        } catch (e: unknown) {
                          console.error(e);
                          setRaidNotice({ variant: 'error', message: t('raid.failMemberFailed') });
                        }
                      }}
                    />
                    {t('raid.failedMember')}
                  </label>
                )}
              </div>
            ))}
            {raidArray.length === 0 && (
              <div className="examiner-empty" data-testid="raid-empty-array" role="status">
                <div className="examiner-icon" aria-hidden="true">
                  <HardDrive size={28} />
                </div>
                <p>{t('raid.emptyArray')}</p>
              </div>
            )}
          </div>

          <div className="raid-footer">
            <button
              type="button"
              className="btn-secondary"
              disabled={raidArray.length < 2 || isDetecting || assembled}
              onClick={detectRaid}
            >
              {isDetecting ? (
                <>
                  <Radar size={18} className="spinner" /> {t('raid.detecting')}
                </>
              ) : (
                <>
                  <Radar size={18} /> {t('raid.detectBtn')}
                </>
              )}
            </button>
            <button
              type="button"
              className="btn-secondary"
              data-testid="raid-assemble-lvm"
              disabled={raidArray.length < 2 || isBuilding || assembled}
              onClick={() => { void assembleLvm() }}
            >
              {isBuilding ? t('raid.lvmBuilding') : t('raid.lvmBtn')}
            </button>
            <button
              type="button"
              className="btn-secondary"
              data-testid="raid-assemble-ldm"
              disabled={raidArray.length < 2 || isBuilding || assembled}
              onClick={() => { void assembleLdm() }}
            >
              {isBuilding ? t('raid.ldmBuilding') : t('raid.ldmBtn')}
            </button>
            <button
              type="button"
              className="btn-primary build-btn"
              disabled={raidArray.length < 2 || isBuilding}
              onClick={buildRaid}
            >
              {isBuilding ? (
                <>
                  <Settings2 size={20} className="spinner" /> {t('raid.building')}
                </>
              ) : (
                <>
                  <Play size={20} fill="currentColor" /> {tFormat('raid.buildBtn', { type: raidType })}
                </>
              )}
            </button>
            <p className="raid-hint">
              {t('raid.hint')}
            </p>
          </div>
        </div>
      </div>
    </div>
  );
};

export default RaidBuilder;
