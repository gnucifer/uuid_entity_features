<?php
/**
 * @file
 * Features hooks for the uuid_entity features component.
 */

/**
 * Implements hook_features_export_options().
 */
function uuid_entity_features_export_options($component) {
  $options = array();
  $type = _uuid_entity_features_component_entity_type($component);
  $entities_settings = variable_get('uuid_entity_features_entities', array());
  if (
    isset($entities_settings[$type]) &&
    !empty($entities_settings[$type]['enabled'])
  ) {
    $query = new EntityFieldQuery();
    $query->entityCondition('entity_type', $type);
    if(!empty($entities_settings[$type]['bundles'])) {
      $query->entityCondition('bundle', array_filter($entities_settings[$type]['bundles']));
      //TODO: order by bundle
    }
    $results = $query->execute();
    if (isset($results[$type])) {
      $entities = entity_load($type, array_keys($results[$type]));
      foreach ($entities as $entity) {
        $wrapped_entity = entity_metadata_wrapper($type, $entity);
        $options[$wrapped_entity->uuid->value()] = t(
          '@bundle: @label',
          array(
            '@bundle' => $wrapped_entity->getBundle(),
            '@label' => $wrapped_entity->label(),
          )
        );
      }
    }
  }
  return $options;
}

/**
 * Implements hook_features_export().
 */
function uuid_entity_features_export($data, &$export, $module_name, $component) {
  $pipe = array();
  $export['dependencies']['uuid_entity_features'] = 'uuid_entity_features';
  $pipe['variable'][] = 'uuid_entity_features_entities';

  $type = _uuid_entity_features_component_entity_type($component);
  $info = entity_get_info($type);
  $info['entity keys'] += array('module' => 'module', 'status' => 'status');
  $status_key = $info['entity keys']['status'];
  $module_key = $info['entity keys']['module'];

  if (!empty($info['bundle of'])) {
    $entity_info = entity_get_info($info['bundle of']);
    $bundle_key = $entity_info['bundle keys']['bundle'];
  }

  foreach($data as $uuid) {
    $export['features'][$component][$uuid] = $uuid;
  }
  //TODO: why not entity_uuid_load?
  //$ids = entity_get_id_by_uuid($type, $data);
  //$entities = entity_load($type, $ids);
  //TODO: use dependancy api when gathering menu item deps?
  $entities = entity_uuid_load($type, $data);
  if($component == 'uuid_entity_menu_link') {
    $ids = entity_get_id_by_uuid($type, $data);
    entity_load($type, $ids);
  }
  foreach ($entities as $entity) {
    // If this is a bundle of a fieldable entity, add its fields to the pipe.
    if (!empty($info['bundle of'])) {
      //TODO: what about fields on current entity bundle?
      $fields = field_info_instances($info['bundle of'], $entity->{$bundle_key});
      foreach ($fields as $name => $field) {
        $pipe['field'][] = "{$field['entity_type']}-{$field['bundle']}-{$field['field_name']}";
      }
    }
  }
  // Add the module providing the entity type as dependency.
  if ($data && !empty($info['module'])) {
    $export['dependencies'][$info['module']] = $info['module'];
    // In case entity is not already an indirect dependency, add it.
    // We can do so without causing redundant dependencies because,
    // if entity is an indirect dependency, Features will filter it out.
    $export['dependencies']['entity'] = 'entity';
  }
  $context = array(
    'data' => $data,
    'module_name' => $module_name,
    'component' => $component,
  );
  //TODO: pipe and export alter already exist, no need to provide own alter?
  drupal_alter(
    array('uuid_entity_features_export', 'uuid_entity_features_' . $type . '_export'),
    $export,
    $pipe,
    $context
  );
  return $pipe;
}

/**
 * Implements hook_features_export_render().
 */
function uuid_entity_features_export_render($module, $data, $export, $component) {
  $type = _uuid_entity_features_component_entity_type($component);
  $info = entity_get_info($type);
  $output = array();
  $output[] = '  $items = array();';

  $ids = entity_get_id_by_uuid($type, $data);
  $ids_guids = array_flip($ids);

  $info = entity_get_info($type);
  $info['entity keys'] += array('module' => 'module', 'status' => 'status');
  $status_key = $info['entity keys']['status'];
  $module_key = $info['entity keys']['module'];
  $id_key = $info['entity keys']['id'];

  $context = array(
    'module_name' => $module,
    'export' => $export,
    'component' => $component,
    'entity_type' => $type,
  );

  foreach (entity_uuid_load($type, $data) as $id => $entity) {
    drupal_alter(
      array(
        'uuid_entity_features_export_render_entity',
        'uuid_entity_features_export_render_' . $type . '_entity',
      ),
      $entity,
      $context
    );

    $export  = "  \$items['{$ids_guids[$id]}'] = entity_create('{$type}', ";
    $vars = get_object_vars($entity);
    //TODO: do not need to unset id and revision id?
    unset($vars[$status_key], $vars[$status_key], $vars[$id_key], $vars['is_new']);
    if(!empty($info['entity keys']['revision'])) {
      unset($vars[$info['entity keys']['revision']]);
    }
    $export .= features_var_export($vars, '  ');
    $export .= ");";

    $output[] = $export;
  }
  $output[] = '  return $items;';
  $output = implode("\n", $output);

  return array('uuid_entity_default_' . $type => $output);
}

/**
 * Implements hook_features_revert().
 */
function uuid_entity_features_revert($module, $component) {
  uuid_entity_features_rebuild($module, $component);
}


/**
 * Implements hook_features_rebuild().
 * Rebuilds entitys based on UUID from code defaults.
 */
function uuid_entity_features_rebuild($module, $component) {
  $type = _uuid_entity_features_component_entity_type($component);
  $entities_settings = uuid_entity_features_entities_settings();
  $settings = $entities_settings[$type];
  $entities = features_get_default($component, $module);

  //TODO: Why not use default features hook, difference?
  $context = array(
    'module_name' => $module,
    'component' => $component,
    'entity_type' => $type,
  );
  drupal_alter(
    array(
      'uuid_entity_features_rebuild_entities',
      'uuid_entity_features_rebuild_' . $type . '_entities',
    ),
    $entities,
    $context
  );

  if (!empty($entities)) {
    //TODO: First sync translations!
    $ids = entity_get_id_by_uuid($type, array_keys($entities));
    //TODO: or in foreach?
    //Rename to existing_entities for consistency?
    $stored_entities = array();
    if(!empty($ids)) {
      if (
        $settings['rebuild_behavior'] == UUID_ENTITY_FEATURES_REBUILD_REPLACE
      ) {
        entity_delete_multiple($type, array_values($ids));
        //TODO: Could uuid module take care of cache invalidation on entity
        //delete hook or similar?
        $cache = &drupal_static('entity_uuid_id_cache', array());
        //TODO: Granular unset per guid?
        unset($cache[$type]);
        $ids = array();
      }
      else {
        $stored_entities = entity_uuid_load($type, array_keys($entities));
      }
    }
    $fields_info = field_info_fields();

    //alter? yes
    //drupal_alter!!!
    //TODO: here we can excude invalid entities on import
    foreach($entities as $uuid => $entity) {
      if(
        isset($ids[$uuid]) &&
        isset($stored_entities[$ids[$uuid]])
      ) {
        if (
          $settings['rebuild_behavior'] == UUID_ENTITY_FEATURES_REBUILD_UPDATE ||
          $settings['rebuild_behavior'] == UUID_ENTITY_FEATURES_REBUILD_UPDATE_NON_EMPTY
        ) {
          $wrapped_entity = entity_metadata_wrapper($type, $entity);
          $stored_entity = $stored_entities[$ids[$uuid]];
          $wrapped_stored_entity = entity_metadata_wrapper($type, $stored_entities[$ids[$uuid]]);
          $field_info_instances = field_info_instances($type, $wrapped_entity->getBundle());

          foreach (get_object_vars($entity) as $property => $value) {
            //Special handling for fields
            if (
              isset($field_info_instances[$property]) &&
              $settings['rebuild_behavior'] == UUID_ENTITY_FEATURES_REBUILD_UPDATE_NON_EMPTY
            ) {
              $value = $entity->{$property};
              foreach ($value as $lang => $items) {
                $value[$lang] = _field_filter_items($fields_info[$property], $items);
              }
              if (!empty($value)) {
                $stored_entity->{$property} = $value + $stored_entity->{$property};
              }
            }
            elseif (isset($stored_entity->{$property})) {
              $stored_entity->{$property} = $value;
            }
          }

          /*
          foreach($wrapped_entity->getPropertyInfo() as $property => $info) {
            if(isset($info['field']) && $info['field']) {
              if(isset($field_info_instances[$property])) {
                $value = $entity->{$property};
                foreach($value as $lang => $items) {
                  $value[$lang] = _field_filter_items($fields_info[$property], $items);
                }
                $value = array_filter($value);
                if(!empty($value)) {
                  //FOR FIELD TRANSLATION SET LANGUAGE!?
                  //Check if target entity has translation!
                  $stored_entity->{$property} = $value;
                }
              }
            }
            else if(
              !empty($info['setter callback']) &&
              !(
                $settings['rebuild_behavior'] == UUID_ENTITY_FEATURES_REBUILD_UPDATE_NON_EMPTY &&
                empty($wrapped_entity->get($property)->value())
              )
            ) {
              //TODO: should not set status, created, author, type, comment?
              try {
                $wrapped_stored_entity->get($property)->set($wrapped_entity->get($property)->value());
              }
              catch (Exception $e) {
                //TODO: improve
                $message = t(
                  'Error setting @property: @error',
                  array(
                    '@property' => $property,
                    '@error' => $e->getMessage()
                  )
                );
                drupal_set_message($message, 'error');
                watchdog_exception('uuid_entity_features', $e);
              }
            }
          }
          */
          //Update
          entity_uuid_save($type, $stored_entity);
        }
      }
      else {
        //Create
        entity_uuid_save($type, $entity);
      }
    }
  }
}
