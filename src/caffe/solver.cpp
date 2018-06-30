#include <cstdio>

#include <string>
#include <vector>

#include "caffe/caffe.hpp"
#include "caffe/solver.hpp"
#include "caffe/util/format.hpp"
#include "caffe/util/hdf5.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/upgrade_proto.hpp"
#include "caffe/adaptive_probabilistic_pruning.hpp"
#include <ctime>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <numeric>

#include "boost/algorithm/string.hpp"

namespace caffe {

template<typename Dtype>
void Solver<Dtype>::SetActionFunction(ActionCallback func) {
  action_request_function_ = func;
}

template<typename Dtype>
SolverAction::Enum Solver<Dtype>::GetRequestedAction() {
  if (action_request_function_) {
    // If the external request function has been set, call it.
    return action_request_function_();
  }
  return SolverAction::NONE;
}

template <typename Dtype>
//- construct solver with SolverParameter
Solver<Dtype>::Solver(const SolverParameter& param, const Solver* root_solver)
    : net_(), callbacks_(), root_solver_(root_solver),
      requested_early_exit_(false) {
  Init(param);
}

template <typename Dtype>
// construct solver with param_file
Solver<Dtype>::Solver(const string& param_file, const Solver* root_solver)
    : net_(), callbacks_(), root_solver_(root_solver),
      requested_early_exit_(false) {
  SolverParameter param;
  ReadSolverParamsFromTextFileOrDie(param_file, &param);
  Init(param);
}

template <typename Dtype>
void Solver<Dtype>::Init(const SolverParameter& param) {
  CHECK(Caffe::root_solver() || root_solver_)
      << "root_solver_ needs to be set for all non-root solvers";
  LOG_IF(INFO, Caffe::root_solver()) << "Initializing solver from parameters: "
    << std::endl << param.DebugString();
  param_ = param;
  
  // ------------------------------------------
  // copy prune params
  APP<Dtype>::prune_method = param_.prune_method();
  if (APP<Dtype>::prune_method != "None") {
      char* mthd = new char[strlen(APP<Dtype>::prune_method.c_str()) + 1];
      strcpy(mthd, APP<Dtype>::prune_method.c_str());
      APP<Dtype>::prune_coremthd = strtok(mthd, "_"); // mthd is like "Reg_Col", the first split is `Reg`
      APP<Dtype>::prune_unit = strtok(NULL, "_"); // TODO: put this in APP's member function

      char* coremthd = new char[strlen(APP<Dtype>::prune_coremthd.c_str()) + 1];
      strcpy(coremthd, APP<Dtype>::prune_coremthd.c_str());
      APP<Dtype>::prune_coremthd_ = strtok(coremthd, "-");
  }
  APP<Dtype>::ratio_once_prune = param_.ratio_once_prune();
  APP<Dtype>::prune_interval = param_.prune_interval();
  APP<Dtype>::clear_history_interval = 1;
  APP<Dtype>::prune_begin_iter = -1;
  APP<Dtype>::AA = param_.aa();
  APP<Dtype>::target_reg = param_.target_reg();
  APP<Dtype>::kk  = 0.25; //param_.kk(); 
  APP<Dtype>::kk2 = 0.1;
  APP<Dtype>::speedup = param_.speedup();
  APP<Dtype>::compRatio = param_.compratio();
  APP<Dtype>::IF_update_row_col = param.if_update_row_col();
  APP<Dtype>::IF_speedup_count_fc = param.if_speedup_count_fc();
  APP<Dtype>::IF_compr_count_conv = param.if_compr_count_conv();
  APP<Dtype>::IF_scheme1_when_Reg_rank = param.if_scheme1_when_reg_rank();
  APP<Dtype>::IF_eswpf = param_.if_eswpf(); /// if early stop when prune finished
  APP<Dtype>::prune_threshold = param_.prune_threshold();
  // APP<Dtype>::mask_generate_mechanism = param_.mask_generate_mechanism();
  // APP<Dtype>::score_decay = param_.score_decay();
  
  APP<Dtype>::iter_size = APP<Dtype>::prune_method == "None" ? param_.iter_size() : param_.iter_size() / 4;
  APP<Dtype>::learning_rate = APP<Dtype>::prune_method == "None" ? param_.base_lr()   : param_.base_lr() * 5;
  APP<Dtype>::accu_borderline = param_.accu_borderline();
  APP<Dtype>::loss_borderline = param_.loss_borderline();
  APP<Dtype>::recovery_interval = param_.recovery_interval();
  APP<Dtype>::losseval_interval = param_.losseval_interval();
  APP<Dtype>::cnt_loss_cross_borderline.resize(min(APP<Dtype>::losseval_interval, 10000), 1);

  const Dtype index[] = {8,7,6,5,4,3,2}; // When speedup or compRatio = 8~2, snapshot.
  APP<Dtype>::when_snapshot.insert(APP<Dtype>::when_snapshot.begin(), index, index + sizeof(index)/sizeof(index[0]));
  // ------------------------------------------

  CHECK_GE(param_.average_loss(), 1) << "average_loss should be non-negative.";
  CheckSnapshotWritePermissions();
  if (Caffe::root_solver() && param_.random_seed() >= 0) {
    Caffe::set_random_seed(param_.random_seed());
  }
  // Scaffolding code
  InitTrainNet();

  if (Caffe::root_solver()) {
    InitTestNets();
    LOG(INFO) << "Solver scaffolding done.";
  }
  iter_ = 0;
  current_step_ = 0;
}

template <typename Dtype>
void Solver<Dtype>::InitTrainNet() {
  const int num_train_nets = param_.has_net() + param_.has_net_param() +
      param_.has_train_net() + param_.has_train_net_param();
  const string& field_names = "net, net_param, train_net, train_net_param";
  CHECK_GE(num_train_nets, 1) << "SolverParameter must specify a train net "
      << "using one of these fields: " << field_names;
  CHECK_LE(num_train_nets, 1) << "SolverParameter must not contain more than "
      << "one of these fields specifying a train_net: " << field_names;
  NetParameter net_param;
  if (param_.has_train_net_param()) {
    LOG_IF(INFO, Caffe::root_solver())
        << "Creating training net specified in train_net_param.";
    net_param.CopyFrom(param_.train_net_param());
  } else if (param_.has_train_net()) {
    LOG_IF(INFO, Caffe::root_solver())
        << "Creating training net from train_net file: " << param_.train_net();
    ReadNetParamsFromTextFileOrDie(param_.train_net(), &net_param);
  }
  if (param_.has_net_param()) {
    LOG_IF(INFO, Caffe::root_solver())
        << "Creating training net specified in net_param.";
    net_param.CopyFrom(param_.net_param());
  }
  if (param_.has_net()) {
    LOG_IF(INFO, Caffe::root_solver())
        << "Creating training net from net file: " << param_.net();
    ReadNetParamsFromTextFileOrDie(param_.net(), &net_param);
    APP<Dtype>::model_prototxt = param_.net();
  }
  
  // Set the correct NetState.  We start with the solver defaults (lowest
  // precedence); then, merge in any NetState specified by the net_param itself;
  // finally, merge in any NetState specified by the train_state (highest
  // precedence).
  NetState net_state;
  net_state.set_phase(TRAIN);
  net_state.MergeFrom(net_param.state());
  net_state.MergeFrom(param_.train_state());
  net_param.mutable_state()->CopyFrom(net_state);
  if (Caffe::root_solver()) {
    net_.reset(new Net<Dtype>(net_param));
  } else {
    net_.reset(new Net<Dtype>(net_param, root_solver_->net_.get()));
  }
}

template <typename Dtype>
void Solver<Dtype>::InitTestNets() {
  CHECK(Caffe::root_solver());
  const bool has_net_param = param_.has_net_param();
  const bool has_net_file = param_.has_net();
  const int num_generic_nets = has_net_param + has_net_file;
  CHECK_LE(num_generic_nets, 1)
      << "Both net_param and net_file may not be specified.";
  const int num_test_net_params = param_.test_net_param_size();
  const int num_test_net_files = param_.test_net_size();
  const int num_test_nets = num_test_net_params + num_test_net_files;
  if (num_generic_nets) {
      CHECK_GE(param_.test_iter_size(), num_test_nets)
          << "test_iter must be specified for each test network.";
  } else {
      CHECK_EQ(param_.test_iter_size(), num_test_nets)
          << "test_iter must be specified for each test network.";
  }
  // If we have a generic net (specified by net or net_param, rather than
  // test_net or test_net_param), we may have an unlimited number of actual
  // test networks -- the actual number is given by the number of remaining
  // test_iters after any test nets specified by test_net_param and/or test_net
  // are evaluated.
  const int num_generic_net_instances = param_.test_iter_size() - num_test_nets;
  const int num_test_net_instances = num_test_nets + num_generic_net_instances;
  if (param_.test_state_size()) {
    CHECK_EQ(param_.test_state_size(), num_test_net_instances)
        << "test_state must be unspecified or specified once per test net.";
  }
  if (num_test_net_instances) {
    if (APP<Dtype>::prune_method != "None" && APP<Dtype>::test_gpu_id != -1) {
      return;
    }
    CHECK_GT(param_.test_interval(), 0);
  }
  int test_net_id = 0;
  vector<string> sources(num_test_net_instances);
  vector<NetParameter> net_params(num_test_net_instances);
  for (int i = 0; i < num_test_net_params; ++i, ++test_net_id) {
      sources[test_net_id] = "test_net_param";
      net_params[test_net_id].CopyFrom(param_.test_net_param(i));
  }
  for (int i = 0; i < num_test_net_files; ++i, ++test_net_id) {
      sources[test_net_id] = "test_net file: " + param_.test_net(i);
      ReadNetParamsFromTextFileOrDie(param_.test_net(i),
          &net_params[test_net_id]);
  }
  const int remaining_test_nets = param_.test_iter_size() - test_net_id;
  if (has_net_param) {
    for (int i = 0; i < remaining_test_nets; ++i, ++test_net_id) {
      sources[test_net_id] = "net_param";
      net_params[test_net_id].CopyFrom(param_.net_param());
    }
  }
  if (has_net_file) {
    for (int i = 0; i < remaining_test_nets; ++i, ++test_net_id) {
      sources[test_net_id] = "net file: " + param_.net();
      ReadNetParamsFromTextFileOrDie(param_.net(), &net_params[test_net_id]);
    }
  }
  test_nets_.resize(num_test_net_instances);
  for (int i = 0; i < num_test_net_instances; ++i) {
    // Set the correct NetState.  We start with the solver defaults (lowest
    // precedence); then, merge in any NetState specified by the net_param
    // itself; finally, merge in any NetState specified by the test_state
    // (highest precedence).
    NetState net_state;
    net_state.set_phase(TEST);
    net_state.MergeFrom(net_params[i].state());
    if (param_.test_state_size()) {
      net_state.MergeFrom(param_.test_state(i));
    }
    net_params[i].mutable_state()->CopyFrom(net_state);
    LOG(INFO)
        << "Creating test net (#" << i << ") specified by " << sources[i];
    if (Caffe::root_solver()) {
      test_nets_[i].reset(new Net<Dtype>(net_params[i]));
    } else {
      test_nets_[i].reset(new Net<Dtype>(net_params[i],
          root_solver_->test_nets_[i].get()));
    }
    test_nets_[i]->set_debug_info(param_.debug_info());
  }
}

template <typename Dtype>
void Solver<Dtype>::Step(int iters) {
  const int start_iter = iter_;
  const int stop_iter = iter_ + iters;
  int average_loss = this->param_.average_loss();
  losses_.clear();
  smoothed_loss_ = 0;
  Dtype max_acc = 0;
  int max_acc_index = 0;
  int max_acc_iter = 0;

  while (iter_ < stop_iter) {
    APP<Dtype>::step_ = iter_ + 1;
    // zero-init the params
    net_->ClearParamDiffs();
    if (param_.test_interval() && iter_ % param_.test_interval() == 0
        && (iter_ > 0 || param_.test_initialization())
        && Caffe::root_solver()) {
      TestAll();
      if (requested_early_exit_) {
        // Break out of the while loop because stop was requested while testing.
        break;
      }
    }
    // std::cout << "call_backs_.size(): " << callbacks_.size() << std::endl;
    for (int i = 0; i < callbacks_.size(); ++i) {
      callbacks_[i]->on_start();
    }
    const bool display = param_.display() && iter_ % param_.display() == 0;
    net_->set_debug_info(display && param_.debug_info());
    // accumulate the loss and gradient
    Dtype loss = 0;

    // Speed check
    cout << "--- Solver begins timing" << endl;
    clock_t t1 = clock();
    
    APP<Dtype>::inner_iter = 0;
    for (int i = 0; i < APP<Dtype>::iter_size; ++i) {  // param_.iter_size();
      loss += net_->ForwardBackward();
      ++ APP<Dtype>::inner_iter;
    }
    cout << "--- after ForwardBackward: " << (double)(clock() - t1) / CLOCKS_PER_SEC << endl;
    
    loss /= APP<Dtype>::iter_size; // param_.iter_size();
    // average the loss across iterations for smoothed reporting
    UpdateSmoothedLoss(loss, start_iter, average_loss);
    
    if (display) {
      // -----------------------------------------------------------------------------------
      // calculate training speed
      const time_t current_time = time(NULL);
      if (APP<Dtype>::last_time == 0) {
          APP<Dtype>::first_time = current_time;
          APP<Dtype>::first_iter = iter_;
      }
      char train_speed[50];
      sprintf(train_speed, "%.3f(%.3f)s/iter", (current_time - APP<Dtype>::last_time ) * 1.0 / param_.display(),
                                               (current_time - APP<Dtype>::first_time) * 1.0 / (iter_ - APP<Dtype>::first_iter));
      APP<Dtype>::last_time = current_time;
      // -----------------------------------------------------------------------------------
      LOG_IF(INFO, Caffe::root_solver()) << "Iteration " << iter_
          << ", smoothed loss = " << smoothed_loss_
          << ", speed = " << train_speed;
      const vector<Blob<Dtype>*>& result = net_->output_blobs();
      int score_index = 0;
      for (int j = 0; j < result.size(); ++j) {
        const Dtype* result_vec = result[j]->cpu_data();
        const string& output_name =
            net_->blob_names()[net_->output_blob_indices()[j]];
        const Dtype loss_weight =
            net_->blob_loss_weights()[net_->output_blob_indices()[j]];
        for (int k = 0; k < result[j]->count(); ++k) {
          ostringstream loss_msg_stream;
          if (loss_weight) {
            loss_msg_stream << " (* " << loss_weight
                            << " = " << loss_weight * result_vec[k] << " loss)";
          }
          LOG_IF(INFO, Caffe::root_solver()) << "    Train net output #"
              << score_index++ << ": " << output_name << " = "
              << result_vec[k] << loss_msg_stream.str();
        }
      }
    }
    
    // 归约梯度，在Update之前，而在Update中有`Iteration, lr`的logging，合理解释了log中就只有一处这个logging
    for (int i = 0; i < callbacks_.size(); ++i) {
      callbacks_[i]->on_gradients_ready();
    }
    
    ApplyUpdate();
    cout << "--- after ApplyUpdate: " << (double)(clock() - t1) / CLOCKS_PER_SEC << endl;

    // -----------------------------------------------------------------
    // Prune finished
    if(APP<Dtype>::IF_current_target_achieved) {
      cout << "[app]\n[app] Current pruning stage finished. Go on training for a little while before checking accuracy. step: " << APP<Dtype>::stage_iter_prune_finished + 1 << endl;
      for (int L = 0; L < APP<Dtype>::layer_index.size(); ++L) {
        if (APP<Dtype>::prune_ratio[L] == 0) { continue; }
        cout << "[app]    " << L << " - pruned_ratio: " << APP<Dtype>::pruned_ratio_col[L] << endl;
      }
      APP<Dtype>::IF_current_target_achieved = false; // Got in here ONLY once.
      
      // Check reg
      map<string, int>::iterator map_it;
      for (int L = 0; L < APP<Dtype>::layer_index.size(); ++L) {
        if (APP<Dtype>::prune_ratio[L] == 0) { continue; }
        string layer_name = "";
        for (map_it = APP<Dtype>::layer_index.begin(); map_it != APP<Dtype>::layer_index.end(); ++map_it) {
          if (map_it->second == L) {
            layer_name = map_it->first;
            break;
          }
        }
        Dtype* muhistory_punish = this->net_->layer_by_name(layer_name)->history_punish()[0]->mutable_cpu_data();
        const int num_col = this->net_->layer_by_name(layer_name)->blobs()[0]->count(1);
        const int num_row = this->net_->layer_by_name(layer_name)->blobs()[0]->shape()[0];
        vector<Dtype> left_reg;
        cout << "[app]    ";
        for (int j = 0; j < num_col; ++j) {
          if (0 < muhistory_punish[j] && muhistory_punish[j] < APP<Dtype>::target_reg) {
            left_reg.push_back(muhistory_punish[j]);
            for (int i = 0; i < num_row; ++i) {
              muhistory_punish[i * num_col + j] = 0;
            }
          }
        }
        cout << L << " - " << left_reg.size() << " columns' left reg not cleared, now cleared:";
        for (int i = 0; i < left_reg.size(); ++i) {
          cout << " " << left_reg[i];
        }
        cout << endl;
      }
    }

    // Estimate accuracy based on loss
    if (APP<Dtype>::IF_acc_recovered == true) {
      const int vec_size = APP<Dtype>::cnt_loss_cross_borderline.size();
      APP<Dtype>::cnt_loss_cross_borderline[iter_ % vec_size] = (smoothed_loss_ < APP<Dtype>::loss_borderline) ? 0 : 1;
    }

    // Check acc based on loss
    if (iter_ - APP<Dtype>::stage_iter_prune_finished == APP<Dtype>::losseval_interval) {
      const int cnt_loss_cross_borderline = accumulate(APP<Dtype>::cnt_loss_cross_borderline.begin(), APP<Dtype>::cnt_loss_cross_borderline.end(), 0);
      CheckPruneState(cnt_loss_cross_borderline < APP<Dtype>::cnt_loss_cross_borderline.size() / 2);
    }

    /*// Check acc based on true acc
    if (APP<Dtype>::IF_acc_recovered == false 
      && iter_ - APP<Dtype>::stage_iter_prune_finished == APP<Dtype>::losseval_interval + ceil(APP<Dtype>::recovery_interval * pow(1.2, APP<Dtype>::cnt_acc_bad))) {
      OfflineTest();
      const Dtype true_val_acc = *min_element(APP<Dtype>::val_accuracy.begin(), APP<Dtype>::val_accuracy.end());
      cout << "[app] Retrain finished, true_val_acc = " << true_val_acc << ", step: " << APP<Dtype>::step_ << endl;
      CheckPruneState(0, true_val_acc);
    }*/
    
    if (APP<Dtype>::IF_acc_recovered == false 
      && iter_ % APP<Dtype>::retrain_test_interval == 0) {
      OfflineTest();
      const Dtype acc1 = *min_element(APP<Dtype>::val_accuracy.begin(), APP<Dtype>::val_accuracy.end());
      const Dtype acc5 = *max_element(APP<Dtype>::val_accuracy.begin(), APP<Dtype>::val_accuracy.end());
      APP<Dtype>::retrain_test_acc1.push_back(acc1);
      APP<Dtype>::retrain_test_acc5.push_back(acc5);
      if (acc1 > max_acc) {
        max_acc = acc1;
        max_acc_index = APP<Dtype>::retrain_test_acc1.size();
        max_acc_iter = iter_;
      }
      // const int window_size = min(APP<Dtype>::retrain_test_acc1.size(), 5);
      // const Dtype ave_acc = accumulate(APP<Dtype>::retrain_test_acc1.end() - window_size, APP<Dtype>::retrain_test_acc1.end(), 0) / window_size;
      cout << "[app] Retrain going on, current acc1 = " << acc1 << ", step: " << APP<Dtype>::step_ << endl;
      if (APP<Dtype>::retrain_test_acc1.size() - max_acc_index > 4) {
        cout << "[app] Retrain finished, final acc1 = " << max_acc
             << " (step: " << max_acc_iter + 1 << "), step: " << iter_ + 1 << endl;
        const string resume_file = param_.snapshot_prefix() + "_iter_" + caffe::format_int(max_acc_iter) + ".solverstate";
        Restore(resume_file.c_str());
        CheckPruneState(0, max_acc);
        APP<Dtype>::retrain_test_acc1.clear();
        APP<Dtype>::retrain_test_acc5.clear();
        max_acc = 0;
        max_acc_index = 0;
        max_acc_iter = 0;
      }
    }

    // Print speedup & compression ratio each iter
    Dtype current_speedup, current_compRatio, GFLOPs_origin, num_param_origin;
    GetPruneProgress(&current_speedup,
                     &current_compRatio,
                     &GFLOPs_origin,
                     &num_param_origin);
    if (start_iter == iter_) {
      cout << "IF_speedup_count_fc: " << APP<Dtype>::IF_speedup_count_fc
           << "  Total GFLOPs_origin: " << GFLOPs_origin
           << " | IF_compr_count_conv: " << APP<Dtype>::IF_compr_count_conv
           << "  Total num_param_origin: " << num_param_origin << endl;
    }
    cout << "**** Step " << APP<Dtype>::step_ << " (after update): " 
         << current_speedup   << "/" << APP<Dtype>::speedup << " "
         << current_compRatio << "/" << APP<Dtype>::compRatio
         << " ****" << "\n" << endl;
    // -----------------------------------------------------------------
    
    // Increment the internal iter_ counter -- its value should always indicate
    // the number of times the weights have been updated.
    ++iter_;
    
    SolverAction::Enum request = GetRequestedAction();

    // Save a snapshot if needed.
    if ((param_.snapshot()
         && iter_ % param_.snapshot() == 0
         && Caffe::root_solver()) ||
         (request == SolverAction::SNAPSHOT)) {
      Snapshot();
    }
    if (SolverAction::STOP == request) {
      requested_early_exit_ = true;
      // Break out of training loop.
      break;
    }
  }
}

template <typename Dtype>
void Solver<Dtype>::GetPruneProgress(Dtype* speedup, Dtype* compRatio, Dtype* GFLOPs_origin_, Dtype* num_param_origin_) {
  // speedup
  Dtype GFLOPs_left   = 0;
  Dtype GFLOPs_origin = 0;
  const int num_layer_count = APP<Dtype>::IF_speedup_count_fc ? APP<Dtype>::layer_index.size() : APP<Dtype>::conv_layer_cnt;
  for (int i = 0; i < num_layer_count; ++i) {
      const Dtype pr = APP<Dtype>::pruned_ratio_row[i];
      const Dtype pc = APP<Dtype>::pruned_ratio_col[i];
      GFLOPs_left   += APP<Dtype>::GFLOPs[i] * (1 - (pr + pc - pr * pc));
      GFLOPs_origin += APP<Dtype>::GFLOPs[i];
  }
  if (APP<Dtype>::prune_unit == "Col" || APP<Dtype>::prune_unit == "Row") {
      APP<Dtype>::IF_speedup_achieved = GFLOPs_origin / GFLOPs_left >= APP<Dtype>::speedup;
  }
  *speedup = GFLOPs_origin / GFLOPs_left;
  *GFLOPs_origin_ = GFLOPs_origin;
  
  // compression ratio
  Dtype num_param_left   = 0;
  Dtype num_param_origin = 0;
  const int num_layer_begin = APP<Dtype>::IF_compr_count_conv ? 0 : APP<Dtype>::conv_layer_cnt;
  for (int i = num_layer_begin; i < APP<Dtype>::layer_index.size(); ++i) {
      num_param_left   += APP<Dtype>::num_param[i] * (1 - APP<Dtype>::pruned_ratio[i]);
      num_param_origin += APP<Dtype>::num_param[i];
  }
  if (APP<Dtype>::prune_unit == "Weight") {
      APP<Dtype>::IF_compRatio_achieved = num_param_origin / num_param_left >= APP<Dtype>::compRatio;
  }
  *compRatio = num_param_origin / num_param_left;
  *num_param_origin_ = num_param_origin;
}

template <typename Dtype>
void Solver<Dtype>::SetTrainSetting(const string& train_state) {
  if (train_state == "prune") {
    APP<Dtype>::IF_acc_recovered = true;
    APP<Dtype>::iter_size = param_.iter_size() / 4;
    for (int i = 0; i < APP<Dtype>::cnt_loss_cross_borderline.size(); ++i) {
      APP<Dtype>::cnt_loss_cross_borderline[i] = 0;
    }
  } else if(train_state == "retrain") {
    APP<Dtype>::IF_acc_recovered = false;
    APP<Dtype>::iter_size = param_.iter_size();
  } else {
    cout << "Wrong: unknown train_state, please check." << endl;
    exit(1);
  }
}

template <typename Dtype>
void Solver<Dtype>::CheckPruneState(const bool& IF_acc_far_from_borderline, const Dtype& true_val_acc) {
  if (true_val_acc == -1) { // check accuracy based on loss
    if (IF_acc_far_from_borderline) {
      cout << "[app] Estimated accuracy **significantly good**, directly start a new pruning stage without retraining. step: " << APP<Dtype>::step_ << endl;
      APP<Dtype>::last_feasible_prune_iter = iter_;
      SetNewCurrentPruneRatio(false);
    } else {
      cout << "[app] Estimated accuracy **NOT significantly good**, retrain to check accuracy before starting a new pruning stage. step: " << APP<Dtype>::step_ << endl;
      SetTrainSetting("retrain");
    }
  } else { // check accuracy based on true accuracy
    if (APP<Dtype>::accu_borderline - true_val_acc > 0.0005) { // accuracy bad
      cout << "[app]    accuracy bad, roll back weights to iter = " << APP<Dtype>::last_feasible_prune_iter << endl;
      if (APP<Dtype>::last_feasible_prune_iter == -1) {
        cout << "[app]    The first pruning stage failed, please decrease the initial prune_ratio." << endl;
        exit(1);
      }
      const string resume_file = param_.snapshot_prefix() + "_iter_" + caffe::format_int(APP<Dtype>::last_feasible_prune_iter) + ".solverstate";
      cout << "[app]    ===== resuming from: " << resume_file << endl;
      SetNewCurrentPruneRatio(true);
      Restore(resume_file.c_str()); // Restore weights after 'current_prune_ratio' update, because restoring will change the 'pruned_ratio'.
    } else { // accuracy good
      Snapshot();
      APP<Dtype>::last_feasible_prune_iter = iter_;
      if (fabs(true_val_acc - APP<Dtype>::accu_borderline) < 0.0005 && (++ APP<Dtype>::cnt_acc_hit) == 6) {
        cout << "[app]    #" << APP<Dtype>::cnt_acc_hit << " - true_val_acc hit" << endl;
        cout << "[app]    All pruning done, stop exploring new current_prune_ratio. Next is all retraining." << endl;
        SetTrainSetting("retrain");
      } else {
        if (APP<Dtype>::cnt_acc_hit) { cout << "[app]    #" << APP<Dtype>::cnt_acc_hit << " - true_val_acc hit" << endl; }
        cout << "[app]    true_val_acc **still good**, start a new pruning stage." << endl;
        SetNewCurrentPruneRatio(false);
      }
    }
  }
}

template <typename Dtype>
void Solver<Dtype>::SetNewCurrentPruneRatio(const bool& IF_roll_back) {
  // Check if all pruning done
  bool all_layer_prune_finished = true;
  for (int L = 0; L < APP<Dtype>::layer_index.size(); ++L) {
    if (APP<Dtype>::pruned_ratio_for_comparison[L] < APP<Dtype>::prune_ratio[L]) {
      all_layer_prune_finished = false;
      break;
    }
  }
  const bool IF_final_target_achieved = all_layer_prune_finished || APP<Dtype>::IF_speedup_achieved || APP<Dtype>::IF_compRatio_achieved;
  if (IF_final_target_achieved) {
    cout << "[app]\n[app] All layer prune finished: step = " << APP<Dtype>::step_;
    if (APP<Dtype>::IF_eswpf) {
      cout << " -- early stopped." << endl;
      requested_early_exit_ = true;
    } else {
      cout << " -- go on to retrain." << endl;
      SetTrainSetting("retrain");
      APP<Dtype>::recovery_interval = INT_MAX; // The last retraining will go on and on.
    }
  } else {
    for (int L = 0; L < APP<Dtype>::layer_index.size(); ++L) {
      if (APP<Dtype>::prune_ratio[L] == 0) { continue; }
      if (IF_roll_back) {
        APP<Dtype>::last_infeasible_prune_ratio[L] = APP<Dtype>::pruned_ratio_for_comparison[L];
        APP<Dtype>::current_prune_ratio[L] = (APP<Dtype>::last_feasible_prune_ratio[L] 
                + APP<Dtype>::last_infeasible_prune_ratio[L]) / 2; // new prune ratio could be improved
      } else {
        APP<Dtype>::last_feasible_prune_ratio[L] = APP<Dtype>::pruned_ratio_for_comparison[L];
        if (APP<Dtype>::last_infeasible_prune_ratio[L] == 0) {
          APP<Dtype>::current_prune_ratio[L] = APP<Dtype>::pruned_ratio_for_comparison[L] + APP<Dtype>::prune_ratio_step[L];
        } else {
          if (APP<Dtype>::last_feasible_prune_ratio[L] >= APP<Dtype>::last_infeasible_prune_ratio[L]) {
            APP<Dtype>::prune_ratio_step[L] *= 0.8; // each time surpassing the infeasible, half the prune_ratio_step
            APP<Dtype>::current_prune_ratio[L] = APP<Dtype>::pruned_ratio_for_comparison[L] + APP<Dtype>::prune_ratio_step[L];
            cout << "[app]    " << L << " - surpassed the last_infeasible_prune_ratio" << endl;
          } else if (APP<Dtype>::last_infeasible_prune_ratio[L] - APP<Dtype>::last_feasible_prune_ratio[L] < 0.01) {
            APP<Dtype>::current_prune_ratio[L] = APP<Dtype>::last_infeasible_prune_ratio[L];
            cout << "[app]    " << L << " - try to surpass the last_infeasible_prune_ratio" << endl;
          } else {
            APP<Dtype>::current_prune_ratio[L] = (APP<Dtype>::last_feasible_prune_ratio[L] 
                  + APP<Dtype>::last_infeasible_prune_ratio[L]) / 2;
          }
        }
      }
      APP<Dtype>::current_prune_ratio[L] = min(APP<Dtype>::current_prune_ratio[L], APP<Dtype>::prune_ratio[L]);
      APP<Dtype>::iter_prune_finished[L] = INT_MAX;
      cout << "[app]    " << L << " - current_prune_ratio: " 
           << APP<Dtype>::last_feasible_prune_ratio[L] << " -> " << APP<Dtype>::current_prune_ratio[L] << endl;
    }
    SetTrainSetting("prune");
  }
}

template <typename Dtype>
void Solver<Dtype>::Solve(const char* resume_file) {
  CHECK(Caffe::root_solver());
  LOG(INFO) << "Solving " << net_->name();
  LOG(INFO) << "Learning Rate Policy: " << param_.lr_policy();

  // Initialize to false every time we start solving.
  requested_early_exit_ = false;

  if (resume_file) {
    LOG(INFO) << "Restoring previous solver status from " << resume_file;
    Restore(resume_file);
  }
  
  // After restoring, calculate GFLOPs and determine whether the prune finished
  // Note that, layer target has been check when restoring solverstate.
  Dtype current_speedup, current_compRatio, GFLOPs_origin, num_param_origin;
  GetPruneProgress(&current_speedup,
                   &current_compRatio,
                   &GFLOPs_origin,
                   &num_param_origin);
  if (APP<Dtype>::IF_speedup_achieved || APP<Dtype>::IF_compRatio_achieved) {
    for (int i = 0; i < APP<Dtype>::layer_index.size(); ++i) {
      APP<Dtype>::iter_prune_finished[i] = -1; 
    }
  }

  // For a network that is trained by the solver, no bottom or top vecs
  // should be given, and we will just provide dummy vecs.
  int start_iter = iter_;
  Step(param_.max_iter() - iter_);

  // If we haven't already, save a snapshot after optimization, unless
  // overridden by setting snapshot_after_train := false
  if (param_.snapshot_after_train()
      && (!param_.snapshot() || iter_ % param_.snapshot() != 0)) {
    Snapshot();
  }
  if (requested_early_exit_) {
    if (APP<Dtype>::prune_method != "None") { PrintFinalPrunedRatio(); }
    LOG(INFO) << "Optimization stopped early.";
    return;
  }
  // After the optimization is done, run an additional train and test pass to
  // display the train and test loss/outputs if appropriate (based on the
  // display and test_interval settings, respectively).  Unlike in the rest of
  // training, for the train net we only run a forward pass as we've already
  // updated the parameters "max_iter" times -- this final pass is only done to
  // display the loss, which is computed in the forward pass.
  if (param_.display() && iter_ % param_.display() == 0) {
    int average_loss = this->param_.average_loss();
    Dtype loss;
    net_->Forward(&loss);
    UpdateSmoothedLoss(loss, start_iter, average_loss);
    LOG(INFO) << "Iteration " << iter_ << ", loss = " << smoothed_loss_;
  }
  if (param_.test_interval() && iter_ % param_.test_interval() == 0) {
    TestAll();
  }
  if (APP<Dtype>::prune_method != "None") { PrintFinalPrunedRatio(); }
  LOG(INFO) << "Optimization Done.";
}

template <typename Dtype>
void Solver<Dtype>::PrintFinalPrunedRatio() {
  cout << "Print final pruned ratio of all layers:" << endl;
  map<string, int>::iterator it_m;
  for (it_m = APP<Dtype>::layer_index.begin(); it_m != APP<Dtype>::layer_index.end(); ++it_m) {
    const int L = it_m->second;
    cout << it_m->first
         << "  pruned_ratio = " << APP<Dtype>::pruned_ratio[L]
         << "  pruned_ratio_row = " << APP<Dtype>::pruned_ratio_row[L]
         << "  pruned_ratio_col = " << APP<Dtype>::pruned_ratio_col[L] 
         << "  prune_ratio = " << APP<Dtype>::prune_ratio[L] << endl;
  }
}

// ----------------------------------------------------------------------------------
template <typename Dtype>
void Solver<Dtype>::OfflineTest() {
  // Create test net
  Snapshot();
  const string& weights = param_.snapshot_prefix() + "_iter_" + caffe::format_int(iter_) + ".caffemodel";
  Net<Dtype> test_net(APP<Dtype>::model_prototxt, caffe::TEST);
  test_net.CopyTrainedLayersFrom(weights);
  
  // Switch GPU
  int gpu_id = APP<Dtype>::test_gpu_id;
  if (gpu_id == -1) {
    gpu_id = APP<Dtype>::original_gpu_id;
  }
  LOG(INFO) << "-------------------------- retrain test begins --------------------------";
  LOG(INFO) << "Use GPU with device ID " << gpu_id;
  cudaDeviceProp device_prop;
  cudaGetDeviceProperties(&device_prop, gpu_id);
  LOG(INFO) << "GPU device name: " << device_prop.name;
  Caffe::SetDevice(gpu_id);
  Caffe::set_mode(Caffe::GPU);
  
  const int num_iter = param_.test_iter(0);
  LOG(INFO) << "Running for " << num_iter << " iterations.";
  APP<Dtype>::val_accuracy.clear();
  vector<int> test_score_output_id;
  vector<Dtype> test_score;
  Dtype loss = 0;
  for (int i = 0; i < num_iter; ++i) {
    Dtype iter_loss;
    const vector<Blob<Dtype>*>& result =
        test_net.Forward(&iter_loss);
    loss += iter_loss;
    int idx = 0;
    for (int j = 0; j < result.size(); ++j) {
      const Dtype* result_vec = result[j]->cpu_data();
      for (int k = 0; k < result[j]->count(); ++k, ++idx) {
        const Dtype score = result_vec[k];
        if (i == 0) {
          test_score.push_back(score);
          test_score_output_id.push_back(j);
        } else {
          test_score[idx] += score;
        }
        // const std::string& output_name = test_net.blob_names()[
            // test_net.output_blob_indices()[j]];
        // LOG(INFO) << "Batch " << i << ", " << output_name << " = " << score;
      }
    }
  }
  loss /= num_iter;
  LOG(INFO) << "Loss: " << loss;
  for (int i = 0; i < test_score.size(); ++i) {
    const std::string& output_name = test_net.blob_names()[
        test_net.output_blob_indices()[test_score_output_id[i]]];
    const Dtype loss_weight = test_net.blob_loss_weights()[
        test_net.output_blob_indices()[test_score_output_id[i]]];
    std::ostringstream loss_msg_stream;
    const Dtype mean_score = test_score[i] / num_iter;
    if (loss_weight) {
      loss_msg_stream << " (* " << loss_weight
                      << " = " << loss_weight * mean_score << " loss)";
    }
    LOG(INFO) << output_name << " = " << mean_score << loss_msg_stream.str();
    if (output_name.substr(1, 3) == "ccu") { // TODO(mingsuntse): improve this
      APP<Dtype>::val_accuracy.push_back(mean_score);
    }
  }
  LOG(INFO) << "-------------------------- retrain test done --------------------------";
  Caffe::SetDevice(APP<Dtype>::original_gpu_id); // Change back to original gpu
}

/// @mingsuntse, divide train net and test net on different GPUs, failed for now.
template <typename Dtype>
void Solver<Dtype>::OfflineTest(int gpu_id, const int& num_iter) {  
  // Create test_net
  NetParameter net_param; 
  ReadNetParamsFromTextFileOrDie(param_.net(), &net_param);
  shared_ptr<Net<Dtype> > test_net;
  test_net.reset(new Net<Dtype>(net_param));
  CHECK_NOTNULL(test_net.get())->ShareTrainedLayersWith(net_.get()); // Copy weights to test_net
  
  // Switch GPU
  if (gpu_id == -1) {
    gpu_id = APP<Dtype>::original_gpu_id;
  }
  LOG(INFO) << "Use GPU with device ID " << gpu_id;
#ifndef CPU_ONLY
  cudaDeviceProp device_prop;
  cudaGetDeviceProperties(&device_prop, gpu_id);
  LOG(INFO) << "GPU device name: " << device_prop.name;
#endif
  Caffe::SetDevice(gpu_id);
  Caffe::set_mode(Caffe::GPU);
  
  // Testing runs
  LOG(INFO) << "Running for " << num_iter << " iterations.";
  APP<Dtype>::val_accuracy.clear();
  vector<int> test_score_output_id;
  vector<Dtype> test_score;
  Dtype loss = 0;
  for (int i = 0; i < num_iter; ++i) {
    Dtype iter_loss;
    const vector<Blob<Dtype>*>& result =
        test_net->Forward(&iter_loss);
    loss += iter_loss;
    int idx = 0;
    for (int j = 0; j < result.size(); ++j) {
      const Dtype* result_vec = result[j]->cpu_data();
      for (int k = 0; k < result[j]->count(); ++k, ++idx) {
        const Dtype score = result_vec[k];
        if (i == 0) {
          test_score.push_back(score);
          test_score_output_id.push_back(j);
        } else {
          test_score[idx] += score;
        }
      }
    }
  }
  loss /= num_iter;
  LOG(INFO) << "Loss: " << loss;
  for (int i = 0; i < test_score.size(); ++i) {
    const std::string& output_name = test_net->blob_names()[
        test_net->output_blob_indices()[test_score_output_id[i]]];
    const Dtype loss_weight = test_net->blob_loss_weights()[
        test_net->output_blob_indices()[test_score_output_id[i]]];
    std::ostringstream loss_msg_stream;
    const Dtype mean_score = test_score[i] / num_iter;
    if (loss_weight) {
      loss_msg_stream << " (* " << loss_weight
                      << " = " << loss_weight * mean_score << " loss)";
    }
    LOG(INFO) << output_name << " = " << mean_score << loss_msg_stream.str();
    if (output_name.substr(1, 3) == "ccu") { // TODO(mingsuntse): improve this
      APP<Dtype>::val_accuracy.push_back(mean_score);
    }
  }
  Caffe::SetDevice(APP<Dtype>::original_gpu_id); // Change back to original gpu
}
// --------------------------------------------------------------------------------

template <typename Dtype>
void Solver<Dtype>::TestAll() {
  for (int test_net_id = 0;
       test_net_id < test_nets_.size() && !requested_early_exit_;
       ++test_net_id) {
    Test(test_net_id);
  }
}

template <typename Dtype>
void Solver<Dtype>::Test(const int test_net_id) {
  APP<Dtype>::val_accuracy.clear();
  CHECK(Caffe::root_solver());  
  LOG(INFO) << "Iteration " << iter_
            << ", Testing net (#" << test_net_id << ")";
  CHECK_NOTNULL(test_nets_[test_net_id].get())->
      ShareTrainedLayersWith(net_.get());
  vector<Dtype> test_score;
  vector<int> test_score_output_id;
  const shared_ptr<Net<Dtype> >& test_net = test_nets_[test_net_id];
  Dtype loss = 0;
  for (int i = 0; i < param_.test_iter(test_net_id); ++i) {
    SolverAction::Enum request = GetRequestedAction();
    // Check to see if stoppage of testing/training has been requested.
    while (request != SolverAction::NONE) {
        if (SolverAction::SNAPSHOT == request) {
          Snapshot();
        } else if (SolverAction::STOP == request) {
          requested_early_exit_ = true;
        }
        request = GetRequestedAction();
    }
    if (requested_early_exit_) {
      // break out of test loop.
      break;
    }

    Dtype iter_loss;
    const vector<Blob<Dtype>*>& result =
        test_net->Forward(&iter_loss);
    if (param_.test_compute_loss()) {
      loss += iter_loss;
    }
    if (i == 0) {
      for (int j = 0; j < result.size(); ++j) {
        const Dtype* result_vec = result[j]->cpu_data();
        for (int k = 0; k < result[j]->count(); ++k) {
          test_score.push_back(result_vec[k]);
          test_score_output_id.push_back(j);
        }
      }
    } else {
      int idx = 0;
      for (int j = 0; j < result.size(); ++j) {
        const Dtype* result_vec = result[j]->cpu_data();
        for (int k = 0; k < result[j]->count(); ++k) {
          test_score[idx++] += result_vec[k];
        }
      }
    }
  }
  if (requested_early_exit_) {
    LOG(INFO) << "Test interrupted.";
    return;
  }
  if (param_.test_compute_loss()) {
    loss /= param_.test_iter(test_net_id);
    LOG(INFO) << "Test loss: " << loss;
  }
  for (int i = 0; i < test_score.size(); ++i) {
    const int output_blob_index =
        test_net->output_blob_indices()[test_score_output_id[i]];
    const string& output_name = test_net->blob_names()[output_blob_index];
    const Dtype loss_weight = test_net->blob_loss_weights()[output_blob_index];
    ostringstream loss_msg_stream;
    const Dtype mean_score = test_score[i] / param_.test_iter(test_net_id);
    if (loss_weight) {
      loss_msg_stream << " (* " << loss_weight
                      << " = " << loss_weight * mean_score << " loss)";
    }
    LOG(INFO) << "    Test net output #" << i << ": " << output_name << " = "
              << mean_score << loss_msg_stream.str();
    if (output_name.substr(1, 3) == "ccu") { // "Accuracy" or "accuracy", TODO(mingsuntse): improve this
      APP<Dtype>::val_accuracy.push_back(mean_score);
    }
  }
}

template <typename Dtype>
void Solver<Dtype>::Snapshot() {
  CHECK(Caffe::root_solver());
  string model_filename;
  switch (param_.snapshot_format()) {
  case caffe::SolverParameter_SnapshotFormat_BINARYPROTO:
    model_filename = SnapshotToBinaryProto(); // save caffemodel
    break;
  case caffe::SolverParameter_SnapshotFormat_HDF5:
    model_filename = SnapshotToHDF5();
    break;
  default:
    LOG(FATAL) << "Unsupported snapshot format.";
  }
  SnapshotSolverState(model_filename); // save solverstate
}

/*
/// @mingsuntse, Deprecated -- use built-in solverstate
template <typename Dtype>
void Solver<Dtype>::PruneStateShot() {
    map<string, int>::iterator it_m;
    const string prune_state_dir = param_.snapshot_prefix() + APP<Dtype>::prune_state_dir;
    if (access(prune_state_dir.c_str(), 0)) {
        LOG(INFO) << "Prune state dir: `" << prune_state_dir << "` doesn't exist, now make it." << endl;
        mkdir(prune_state_dir.c_str(), S_IRWXU);
    }
    for (it_m = APP<Dtype>::layer_index.begin(); it_m != APP<Dtype>::layer_index.end(); ++it_m) {
        if (APP<Dtype>::iter_prune_finished[it_m->second] != INT_MAX) { continue; }
        const string outfile = param_.snapshot_prefix() + APP<Dtype>::prune_state_dir + it_m->first + ".txt";
        ofstream state_stream(outfile.c_str(), ios::out);
        if (!state_stream.is_open()) {
            LOG(INFO) << "Error: cannot open file `" << outfile << "`" << endl;
        } else {
            state_stream << iter_ << "\n";
            if (APP<Dtype>::prune_coremthd.substr(0, 2) == "PP") {
                vector<Dtype> state_punish = APP<Dtype>::history_prob[it_m->second];
                typename vector<Dtype>::iterator it;
                for (it = state_punish.begin(); it != state_punish.end(); ++it) {
                    state_stream << *it << " ";
                }
            } else if (APP<Dtype>::prune_coremthd.substr(0, 3) == "Reg") {
                vector<Dtype> state_score  = APP<Dtype>::hrank[it_m->second];
                vector<Dtype> state_punish = APP<Dtype>::history_reg[it_m->second];
                typename vector<Dtype>::iterator it;
                assert (state_score.size() == state_punish.size());
                for (it = state_score.begin(); it != state_score.end(); ++it) {
                    state_stream << *it << " ";
                }
                for (it = state_punish.begin(); it != state_punish.end(); ++it) {
                    state_stream << *it << " ";
                }
            }
            state_stream.close();
            LOG(INFO) << APP<Dtype>::layer_index[it_m->first] << " " << it_m->first << ": save prune state done!";
        }
    }
}

/// @mingsuntse, Deprecated
template <typename Dtype>
void Solver<Dtype>::Logshot() {
    const time_t t = time(NULL);
    struct tm* ctime = localtime(&t);
    ostringstream TIME;
    TIME << 1900 + ctime->tm_year;
    if (ctime->tm_mon < 9) { TIME << 0; }     
    TIME << 1 + ctime->tm_mon
         << ctime->tm_mday
         << "-"
         << ctime->tm_hour
         << ctime->tm_min;
    
    const string i_tmp = param_.snapshot_prefix() + TIME.str() + "_log_index.txt";
    const string w_tmp = param_.snapshot_prefix() + TIME.str() + "_log_weight.txt";
    const string d_tmp = param_.snapshot_prefix() + TIME.str() + "_log_diff.txt";
    const char* ii = i_tmp.c_str(); 
    const char* ww = w_tmp.c_str(); 
    const char* dd = d_tmp.c_str(); 
    ofstream log_i(ii, ofstream::app);
    ofstream log_w(ww, ofstream::app); 
    ofstream log_d(dd, ofstream::app);
    
    typename vector<vector<vector<Dtype> > >::iterator it_l; /// it_layer
    typename vector<vector<Dtype> >::iterator it_w; /// it_weight
    typename vector<Dtype>::iterator it_i; /// it_iter
    vector<vector<int> >::iterator it_il;
    vector<int>::iterator it_iw;

    if (!log_i.is_open()) { 
        cout << "Error: opening file failed: " << ii << endl; 
    } else {
        for (it_il = APP<Dtype>::log_index.begin(); it_il != APP<Dtype>::log_index.end(); ++it_il) {
            for (it_iw = (*it_il).begin(); it_iw != (*it_il).end(); ++it_iw) {
                log_i << *it_iw << " ";
            }
            log_i << "\n";
        }
    }
    
    if (!log_w.is_open()) { 
        cout << "Error: opening file failed: " << ww << endl; 
    } else {
        for (it_l = APP<Dtype>::log_weight.begin(); it_l != APP<Dtype>::log_weight.end(); ++it_l) {
            for (it_w = (*it_l).begin(); it_w != (*it_l).end(); ++it_w) {
                for (it_i = (*it_w).begin(); it_i != (*it_w).end(); ++it_i) {
                    log_w << *it_i << " ";
                }
                log_w << "\n";
            }
            log_w << "\n";
        }
    }

    if (!log_d.is_open()) { 
        cout << "Error: opening file failed: " << dd << endl; 
    } else {
        for (it_l = APP<Dtype>::log_diff.begin(); it_l != APP<Dtype>::log_diff.end(); ++it_l) {
            for (it_w = (*it_l).begin(); it_w != (*it_l).end(); ++it_w) {
                for (it_i = (*it_w).begin(); it_i != (*it_w).end(); ++it_i) {
                    log_d << *it_i << " ";
                }
                log_d << "\n";
            }
            log_d << "\n";
        }
    }

}
*/

template <typename Dtype>
void Solver<Dtype>::CheckSnapshotWritePermissions() {
  if (Caffe::root_solver() && param_.snapshot()) {
    CHECK(param_.has_snapshot_prefix())
        << "In solver params, snapshot is specified but snapshot_prefix is not";
    string probe_filename = SnapshotFilename(".tempfile");
    std::ofstream probe_ofs(probe_filename.c_str());
    if (probe_ofs.good()) {
      probe_ofs.close();
      std::remove(probe_filename.c_str());
    } else {
      LOG(FATAL) << "Cannot write to snapshot prefix '"
          << param_.snapshot_prefix() << "'.  Make sure "
          << "that the directory exists and is writeable.";
    }
  }
}

template <typename Dtype>
string Solver<Dtype>::SnapshotFilename(const string extension) {
  return param_.snapshot_prefix() + "_iter_" + caffe::format_int(iter_)
    + extension;
}

template <typename Dtype>
string Solver<Dtype>::SnapshotToBinaryProto() {
  string model_filename = SnapshotFilename(".caffemodel");
  LOG(INFO) << "Snapshotting to binary proto file " << model_filename;
  NetParameter net_param;
  net_->ToProto(&net_param, param_.snapshot_diff());
  WriteProtoToBinaryFile(net_param, model_filename);
  return model_filename;
}

template <typename Dtype>
string Solver<Dtype>::SnapshotToHDF5() {
  string model_filename = SnapshotFilename(".caffemodel.h5");
  LOG(INFO) << "Snapshotting to HDF5 file " << model_filename;
  net_->ToHDF5(model_filename, param_.snapshot_diff());
  return model_filename;
}

template <typename Dtype>
void Solver<Dtype>::Restore(const char* state_file) {
  CHECK(Caffe::root_solver());
  string state_filename(state_file);
  if (state_filename.size() >= 3 &&
      state_filename.compare(state_filename.size() - 3, 3, ".h5") == 0) {
    RestoreSolverStateFromHDF5(state_filename);
  } else {
    RestoreSolverStateFromBinaryProto(state_filename);
  }
}

template <typename Dtype>
void Solver<Dtype>::UpdateSmoothedLoss(Dtype loss, int start_iter,
    int average_loss) {
  if (losses_.size() < average_loss) {
    losses_.push_back(loss);
    int size = losses_.size();
    smoothed_loss_ = (smoothed_loss_ * (size - 1) + loss) / size;
  } else {
    int idx = (iter_ - start_iter) % average_loss;
    smoothed_loss_ += (loss - losses_[idx]) / average_loss;
    losses_[idx] = loss;
  }
}

INSTANTIATE_CLASS(Solver);

}  // namespace caffe
