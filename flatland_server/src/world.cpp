/*
 *  ______                   __  __              __
 * /\  _  \           __    /\ \/\ \            /\ \__
 * \ \ \L\ \  __  __ /\_\   \_\ \ \ \____    ___\ \ ,_\   ____
 *  \ \  __ \/\ \/\ \\/\ \  /'_` \ \ '__`\  / __`\ \ \/  /',__\
 *   \ \ \/\ \ \ \_/ |\ \ \/\ \L\ \ \ \L\ \/\ \L\ \ \ \_/\__, `\
 *    \ \_\ \_\ \___/  \ \_\ \___,_\ \_,__/\ \____/\ \__\/\____/
 *     \/_/\/_/\/__/    \/_/\/__,_ /\/___/  \/___/  \/__/\/___/
 * @copyright Copyright 2017 Avidbots Corp.
 * @name	 world.cpp
 * @brief	 Loads world file
 * @author Joseph Duchesne
 *
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2017, Avidbots Corp.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Avidbots Corp. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

#include <Box2D/Box2D.h>
#include <flatland_server/debug_visualization.h>
#include <flatland_server/exceptions.h>
#include <flatland_server/world.h>
#include <ros/ros.h>
#include <yaml-cpp/yaml.h>
#include <boost/filesystem.hpp>
#include <map>
#include <string>

namespace flatland_server {

World::World() : gravity_(0, 0), service_manager_(ServiceManager(this)) {
  physics_world_ = new b2World(gravity_);
  physics_world_->SetContactListener(this);
}

World::~World() {
  ROS_INFO_NAMED("World", "Destroying world...");

  // The order of things matters in the destructor. The contact listener is
  // removed first to avoid the triggering the contact functions in plugin
  // manager which might cause it to work with deleted layers/models.
  physics_world_->SetContactListener(nullptr);

  // the physics body of layers are set to null because there are tons of
  // fixtures in a layer and it is too slow for the destroyBody method to remove
  // them since the AABB tree gets restructured everytime a fixture is removed
  // The memory will later be freed by deleting the world
  for (int i = 0; i < layers_.size(); i++) {
    layers_[i]->body_->physics_body_ = nullptr;
    delete layers_[i];
  }

  // The bodies of models are not set to null like layers because there aren't
  // nearly as many fixtures, and we might hide some memory problems by using
  // the shortcut
  for (int i = 0; i < models_.size(); i++) {
    delete models_[i];
  }

  // This frees the entire Box2D world with everything in it
  delete physics_world_;

  ROS_INFO_NAMED("World", "World destroyed");
}

void World::Update(Timekeeper &timekeeper) {
  plugin_manager_.BeforePhysicsStep(timekeeper);
  physics_world_->Step(timekeeper.GetStepSize(), 10, 10);
  timekeeper.StepTime();
  plugin_manager_.AfterPhysicsStep(timekeeper);
}

void World::BeginContact(b2Contact *contact) {
  plugin_manager_.BeginContact(contact);
}

void World::EndContact(b2Contact *contact) {
  plugin_manager_.EndContact(contact);
}

void World::PreSolve(b2Contact *contact, const b2Manifold *oldManifold) {
  plugin_manager_.PreSolve(contact, oldManifold);
}

void World::PostSolve(b2Contact *contact, const b2ContactImpulse *impulse) {
  plugin_manager_.PostSolve(contact, impulse);
}

World *World::MakeWorld(const std::string &yaml_path) {
  // parse the world YAML file
  YAML::Node yaml;

  try {
    yaml = YAML::LoadFile(yaml_path);
  } catch (const YAML::Exception &e) {
    throw YAMLException("Error loading \"" + yaml_path + "\"", e);
  }

  if (yaml["properties"] && yaml["properties"].IsMap()) {
    // TODO (Chunshang): parse properties
  } else {
    throw YAMLException("Missing/invalid world param \"properties\"");
  }

  World *w = new World();

  try {
    w->LoadLayers(yaml_path);
    w->LoadModels(yaml_path);
  } catch (const YAML::Exception &e) {
    ROS_FATAL_NAMED("World", "Error loading from YAML");
    delete w;
    throw YAMLException(e);
  } catch (const PluginException &e) {
    ROS_FATAL_NAMED("World", "Error loading plugins");
    delete w;
    throw e;
  }
  return w;
}

void World::LoadLayers(const std::string &yaml_path) {
  boost::filesystem::path path(yaml_path);

  YAML::Node yaml;

  try {
    yaml = YAML::LoadFile(path.string());
  } catch (const YAML::Exception &e) {
    throw YAMLException("Error loading \"" + path.string() + "\"", e);
  }

  if (!yaml["layers"] || !yaml["layers"].IsSequence()) {
    throw YAMLException("Missing/invalid world param \"layers\"");
  }

  // loop through each layer and parse the data
  for (int i = 0; i < yaml["layers"].size(); i++) {
    Layer *layer;

    if (cfr_.IsLayersFull()) {
      throw YAMLException("Max number of layers reached, max is " +
                          std::to_string(cfr_.MAX_LAYERS));
    }

    layer = Layer::MakeLayer(physics_world_, &cfr_, path.parent_path(),
                             yaml["layers"][i]);

    layers_.push_back(layer);

    ROS_INFO_NAMED("World", "Layer %s loaded", layer->name_.c_str());
  }
}

void World::LoadModels(const std::string &yaml_path) {
  boost::filesystem::path path(yaml_path);
  YAML::Node yaml;

  try {
    yaml = YAML::LoadFile(path.string());
  } catch (const YAML::Exception &e) {
    throw YAMLException("Error loading \"" + path.string() + "\"", e);
  }

  // models is optional
  if (yaml["models"] && !yaml["models"].IsSequence()) {
    throw YAMLException("Invalid world param \"layers\", must be a sequence");
  } else if (yaml["models"]) {
    for (int i = 0; i < yaml["models"].size(); i++) {
      const YAML::Node &node = yaml["models"][i];
      std::string name, ns;
      std::array<double, 3> pose;
      boost::filesystem::path model_path;

      if (node["name"]) {
        name = node["name"].as<std::string>();
      } else {
        throw YAMLException("Missing model name in model index=" +
                            std::to_string(i));
      }

      if (node["namespace"]) {
        ns = node["namespace"].as<std::string>();
      } else {
        ns = "";
      }

      if (node["pose"] && node["pose"].IsSequence() &&
          node["pose"].size() == 3) {
        pose[0] = node["pose"][0].as<double>();
        pose[1] = node["pose"][1].as<double>();
        pose[2] = node["pose"][2].as<double>();
      } else {
        throw YAMLException("Missing/invalid \"pose\" in " + name + " model");
      }

      if (node["model"]) {
        model_path = boost::filesystem::path(node["model"].as<std::string>());
      } else {
        throw YAMLException("Missing \"model\" in " + name + " model");
      }

      if (model_path.string().front() != '/') {
        model_path = path.parent_path() / model_path;
      }

      LoadModel(model_path.string(), ns, name, pose);
    }
  }
}

void World::LoadModel(const std::string &model_yaml_path, const std::string &ns,
                      const std::string &name,
                      const std::array<double, 3> pose) {
  Model *m = Model::MakeModel(physics_world_, &cfr_, model_yaml_path, ns, name);
  m->TransformAll(pose);
  models_.push_back(m);

  // load model plugins, it is okay to have no plugins
  if (m->plugins_node_ && !m->plugins_node_.IsSequence()) {
    throw YAMLException("Invalid \"plugins\" in " + m->name_ +
                        " model, not a list");
  } else if (m->plugins_node_) {
    for (const auto &plugin_node : m->plugins_node_) {
      plugin_manager_.LoadModelPlugin(m, plugin_node);
    }
  }

  ROS_INFO_NAMED("World", "Model %s loaded", m->name_.c_str());
}

void World::DebugVisualize(bool update_layers) {
  if (update_layers) {
    for (auto &layer : layers_) {
      layer->DebugVisualize();
    }
  }

  for (auto &model : models_) {
    model->DebugVisualize();
  }
}
};  // namespace flatland_server