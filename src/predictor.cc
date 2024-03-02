#include "predictor.h"

#include "predict_engine.h"
#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/menu.h>
#include <rime/segmentation.h>
#include <rime/service.h>
#include <rime/translation.h>
#include <rime/schema.h>
#include <rime/dict/db_pool_impl.h>

namespace rime {

Predictor::Predictor(const Ticket& ticket, an<PredictEngine> predict_engine)
    : Processor(ticket), predict_engine_(predict_engine) {
  // update prediction on context change.
  auto* context = engine_->context();
  select_connection_ = context->select_notifier().connect(
      [this](Context* ctx) { OnSelect(ctx); });
  context_update_connection_ = context->update_notifier().connect(
      [this](Context* ctx) { OnContextUpdate(ctx); });
  option_update_connection_ = context->option_update_notifier().connect(
      [this](Context* ctx, const string& option) {
        OnOptionUpdate(ctx, option);
      });
  if (Schema* schema = engine_->schema()) {
    selectors_ = schema->select_keys();
    if (Config* config = schema->config()) {
      if (!config->GetString("speller/initials", &initials_))
        config->GetString("speller/alphabet", &initials_);
    }
  }
}

Predictor::~Predictor() {
  select_connection_.disconnect();
  context_update_connection_.disconnect();
  option_update_connection_.disconnect();
}

ProcessResult Predictor::ProcessKeyEvent(const KeyEvent& key_event) {
  if (!engine_)
    return kNoop;
  auto keycode = key_event.keycode();
  Context* ctx = engine_->context();
  if (!predict_engine_ || !ctx || !ctx->get_option("prediction"))
    return kNoop;
  if (ctx->composition().empty()) {
    last_action_ = kInitiate;
    if (iteration_counter_ > 0) {
      predict_engine_->Clear();
      iteration_counter_ = 0;
    }
  } else if (keycode == XK_BackSpace) {
    last_action_ = kDelete;
    if (ctx->composition().back().HasTag("prediction")) {
      predict_engine_->Clear();
      ctx->composition().pop_back();
      iteration_counter_ -= 1;
      return kAccepted;
    }
  } else if (keycode == XK_Escape) {
    last_action_ = kDelete;
    if (ctx->composition().back().HasTag("prediction")) {
      predict_engine_->Clear();
      iteration_counter_ = 0;
      if (ctx->HasMenu() && ctx->input().length() > 0) {
        ctx->composition().back().Clear();
      } else {
        ctx->Clear();
      }
      return kAccepted;
    }
  } else if ((keycode == XK_Return || keycode == XK_KP_Enter) &&
             key_event.modifier() == 0 && !ctx->get_option("_auto_commit")) {
    last_action_ = kSelect;
    if (ctx->composition().back().HasTag("prediction")) {
      ctx->composition().back().Clear();
    }
    predict_engine_->Clear();
    iteration_counter_ = 0;
    ctx->Commit();
    return kAccepted;
  } else if (!selectors_.empty() && keycode >= 0x20 && keycode < 0x7f &&
             !key_event.modifier() &&
             selectors_.find((char)keycode) != string::npos) {
    if (ctx->composition().back().HasTag("prediction")) {
      int page_size = engine_->schema()->page_size();
      int index = selectors_.find((char)keycode);
      int page_start = (ctx->composition().back().selected_index / page_size) * page_size;
      if (index < page_size && ctx->Select(page_start + index)) {
        last_action_ = kSelect;
        return kAccepted;
      }
    }
  } else if (selectors_.empty() && !key_event.modifier() &&
             ((keycode >= XK_0 && keycode <= XK_9) ||
              (keycode >= XK_KP_0 && keycode <= XK_KP_9))) {
    if (ctx->composition().back().HasTag("prediction")) {
      int page_size = engine_->schema()->page_size();
      int index = (keycode % 0x10 + 9) % 10;
      int page_start = (ctx->composition().back().selected_index / page_size) * page_size;
      if (index < page_size && ctx->Select(page_start + index)) {
        last_action_ = kSelect;
        return kAccepted;
      }
    }
  } else {
    last_action_ = kUnspecified;
    auto seg = ctx->composition().rbegin();
    if (seg->HasTag("prediction") && keycode > 0x20 && keycode < 0x7f &&
        initials_.find((char)keycode) != string::npos) {
      ctx->composition().back().Clear();
      if (++seg != ctx->composition().rend() && seg->HasTag("prediction")) {
        predict_engine_->Clear();
        iteration_counter_ = 0;
        ctx->Commit();
      }
    }
  }
  return kNoop;
}

// predictor for fluid_editor (confirm the rightmost segment)
void Predictor::OnSelect(Context* ctx) {
  last_action_ = kSelect;
  if (!predict_engine_ || !ctx || !ctx->get_option("prediction") ||
      ctx->get_option("_auto_commit"))
    return;
  auto seg = ctx->composition().rbegin();
  size_t end = ctx->input().length();
  if (seg->end != end || seg->end != seg->start)
    return;
  if (seg->status == Segment::kConfirmed && seg->HasTag("prediction")) {
    const string& text = ctx->GetSelectedCandidate()->text();
    iteration_counter_++;
    ctx->composition().push_back(Segment(end, end));
    int max_iterations = predict_engine_->max_iterations();
    if (max_iterations > 0 && iteration_counter_ >= max_iterations) {
      predict_engine_->Clear();
      iteration_counter_ = 0;
      return;
    }
    PredictAndUpdate(ctx, text);
  } else if ((++seg) != ctx->composition().rend() &&
             seg->status == Segment::kConfirmed) {
    auto cand = seg->GetSelectedCandidate();
    if (!cand || cand->type() == "punct") {
      predict_engine_->Clear();
      iteration_counter_ = 0;
      return;
    }
    PredictAndUpdate(ctx, cand->text());
  }
}

void Predictor::OnOptionUpdate(Context* ctx, const string& option) {
  if (option != "ascii_mode" || !ctx || !ctx->get_option("prediction"))
    return;
  iteration_counter_ = 0;
  if (!ctx->composition().empty() &&
      ctx->composition().back().HasTag("prediction")) {
    if (ctx->get_option("_auto_commit")) {
      ctx->composition().clear();
    } else {
      ctx->composition().pop_back();
    }
  }
}

// predictor for express_editor (commit -> empty composition)
void Predictor::OnContextUpdate(Context* ctx) {
  if (self_updating_ || !predict_engine_ || !ctx ||
      !ctx->get_option("prediction") || !ctx->get_option("_auto_commit") ||
      !ctx->composition().empty() || ctx->commit_history().empty() ||
      last_action_ == kDelete || last_action_ == kInitiate) {
    return;
  }
  LOG(INFO) << "Predictor::OnContextUpdate";
  auto last_commit = ctx->commit_history().back();
  if (last_commit.type == "punct" || last_commit.type == "raw" ||
      last_commit.type == "thru") {
    predict_engine_->Clear();
    iteration_counter_ = 0;
    return;
  }
  if (last_commit.type == "prediction") {
    int max_iterations = predict_engine_->max_iterations();
    iteration_counter_++;
    if (max_iterations > 0 && iteration_counter_ >= max_iterations) {
      predict_engine_->Clear();
      iteration_counter_ = 0;
      return;
    }
  }
  PredictAndUpdate(ctx, last_commit.text);
}

void Predictor::PredictAndUpdate(Context* ctx, const string& context_query) {
  if (predict_engine_->Predict(ctx, context_query)) {
    predict_engine_->CreatePredictSegment(ctx);
    self_updating_ = true;
    ctx->update_notifier()(ctx);
    self_updating_ = false;
  }
}

PredictorComponent::PredictorComponent(
    an<PredictEngineComponent> engine_factory)
    : engine_factory_(engine_factory) {}

PredictorComponent::~PredictorComponent() {}

Predictor* PredictorComponent::Create(const Ticket& ticket) {
  return new Predictor(ticket, engine_factory_->GetInstance(ticket));
}

}  // namespace rime
