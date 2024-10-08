/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package org.apache.arrow.flight;

import org.apache.arrow.flight.impl.Flight;

/** POJO wrapper around protocol specifics for Flight actions. */
public class ActionType {
  private final String type;
  private final String description;

  /**
   * Construct a new instance.
   *
   * @param type The type of action to perform
   * @param description The description of the type.
   */
  public ActionType(String type, String description) {
    super();
    this.type = type;
    this.description = description;
  }

  /** Constructs a new instance from the corresponding protocol buffer object. */
  ActionType(Flight.ActionType type) {
    this.type = type.getType();
    this.description = type.getDescription();
  }

  public String getType() {
    return type;
  }

  public String getDescription() {
    return description;
  }

  /** Converts the POJO to the corresponding protocol buffer type. */
  Flight.ActionType toProtocol() {
    return Flight.ActionType.newBuilder().setType(type).setDescription(description).build();
  }

  @Override
  public String toString() {
    return "ActionType{" + "type='" + type + '\'' + ", description='" + description + '\'' + '}';
  }
}
