"""CPU training entry point. Test samples never participate in model selection."""

import argparse
import hashlib
import json
import random
import time
from pathlib import Path

import numpy as np
import torch
from torch.utils.data import DataLoader

from .data import FusionDataset, collate, validate_dataset
from .inference import decode_detections, point_metrics
from .loss import fusion_loss
from .model import FusionYoloXNano


@torch.no_grad()
def evaluate(model, loader):
    model.eval()
    outputs, truths = [], []
    for images, labels, _ in loader:
        for output, label in zip(model(images).numpy(), labels):
            outputs.append(output)
            truths.append(label["points"][label["active"] > 0.5].numpy())
    best, best_key, selected = None, None, None
    # Thresholds are chosen on validation only and exported with the frozen
    # model. The held-out test set is never inspected or tuned here.
    for confidence in (0.05, 0.1, 0.15, 0.2, 0.3):
        for activity in (0.35, 0.5, 0.65, 0.8):
            predictions = [decode_detections(output, confidence, activity) for output in outputs]
            metrics = point_metrics(predictions, truths)
            key = (metrics["f1"], metrics["precision"], metrics["exact_count_rate"], confidence, activity)
            if best_key is None or key > best_key:
                best, best_key = metrics, key
                selected = {"confidence": confidence, "activity": activity, "selected_on": "val_only"}
    return best, selected


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--epochs", type=int, default=30)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--seed", type=int, default=20261002)
    parser.add_argument("--learning-rate", type=float, default=0.001)
    parser.add_argument("--resume")
    args = parser.parse_args()
    if args.epochs < 1 or args.batch_size < 1 or args.threads < 1:
        parser.error("Epochs, batch size and CPU threads must be positive")
    torch.set_num_threads(args.threads)
    torch.manual_seed(args.seed)
    np.random.seed(args.seed)
    random.seed(args.seed)
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    if (output / "training.json").exists():
        raise ValueError("Refusing to overwrite an existing training run; choose a new output")
    dataset_provenance = validate_dataset(args.dataset)
    training = FusionDataset(args.dataset, "train")
    validation = FusionDataset(args.dataset, "val")
    if set(training.files) & set(validation.files):
        raise ValueError("Training and validation samples overlap")
    loader = DataLoader(training, batch_size=args.batch_size, shuffle=True,
                        num_workers=0, collate_fn=collate)
    val_loader = DataLoader(validation, batch_size=args.batch_size, collate_fn=collate)
    model = FusionYoloXNano()
    if args.resume:
        if (Path(args.resume).parent / "NOT_FOR_PERFORMANCE.txt").exists():
            raise ValueError("Rejected development/interface-only weights cannot initialize a release training run")
        model.load_state_dict(torch.load(args.resume, map_location="cpu", weights_only=True)["model"])
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.learning_rate, weight_decay=1e-4)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, args.epochs, eta_min=args.learning_rate * 0.05)
    history, best = [], None
    start = time.monotonic()
    configuration = vars(args) | {"torch_version": str(torch.__version__), "device": "cpu",
                                 "train_samples": len(training), "val_samples": len(validation),
                                 "test_samples_used_for_training_or_selection": 0,
                                 "domain": "simulation_only", "parameters": sum(p.numel() for p in model.parameters()),
                                 "dataset_provenance": dataset_provenance,
                                 "training_source_sha256": {
                                     name: hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
                                     for name in ("model.py", "loss.py", "data.py", "inference.py", "train.py")}}
    for epoch in range(args.epochs):
        model.train()
        totals, batches = {}, 0
        for images, labels, _ in loader:
            optimizer.zero_grad(set_to_none=True)
            values = fusion_loss(model(images, raw=True), labels, model)
            if not torch.isfinite(values["total"]):
                raise RuntimeError("Nonfinite training loss; no model is exported")
            values["total"].backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 10.0)
            optimizer.step()
            for key, value in values.items():
                totals[key] = totals.get(key, 0.0) + float(value.detach())
            batches += 1
        scheduler.step()
        metrics, thresholds = evaluate(model, val_loader)
        record = {"epoch": epoch + 1, "losses": {k: v / batches for k, v in totals.items()},
                  "validation": metrics, "thresholds": thresholds, "elapsed_seconds": time.monotonic() - start}
        history.append(record)
        print(json.dumps(record), flush=True)
        checkpoint = {"model": model.state_dict(), "epoch": epoch + 1, "configuration": configuration,
                      "validation": metrics, "thresholds": thresholds}
        torch.save(checkpoint, output / "last.pt")
        p95 = metrics["matched_point_error_px_p95"]
        median = metrics["matched_point_error_px_median"]
        selection = (metrics["f1"], -(p95 if p95 is not None else float("inf")),
                     -(median if median is not None else float("inf")))
        if best is None or selection > best:
            best = selection
            torch.save(checkpoint, output / "best.pt")
        (output / "training.json").write_text(json.dumps({"configuration": configuration, "history": history},
                                                        ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
