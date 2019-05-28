#include "gcode_wiper.h"
#include "utilities.h"
#include "logging.h"

gcode_wiper::gcode_wiper()
{
	// Initialize members
	total_distance_ = 0.0;
	previous_total_distance_ = 0.0;
	p_starting_position_ = NULL;
	p_previous_starting_position_ = NULL;
	is_initialized_ = false;
	use_full_wipe_ = true;
	
}

gcode_wiper::gcode_wiper(gcode_wiper_args args)
{
	// Initialize members
	total_distance_ = 0.0;
	previous_total_distance_ = 0.0;
	p_starting_position_ = NULL;
	p_previous_starting_position_ = NULL;
	is_initialized_ = false;
	use_full_wipe_ = true;
	// Set members from parameters
	settings_ = args;
	// Initialize the wiper
	initialize();
}

void gcode_wiper::initialize()
{
	// Make sure the retract before/after wipe percent is not less than zero
	if (utilities::less_than(settings_.retract_before_wipe_percent, 0.0))
	{
		settings_.retract_before_wipe_percent = 0.0;
	}
	if (utilities::less_than(settings_.retract_after_wipe_percent, 0.0))
	{
		settings_.retract_after_wipe_percent = 0.0;
	}
	// If the pre/post wipe retraction percent sum is greater than 1 (100%), reduce each value proportionally.
	double total_retraction_percent = settings_.retract_before_wipe_percent + settings_.retract_after_wipe_percent;
	if (utilities::greater_than(total_retraction_percent, 1.0) && !utilities::is_zero(total_retraction_percent))
	{
		const double reduction_ratio = 1.0 / total_retraction_percent;
		// Set to the defaults
		settings_.retract_before_wipe_percent = reduction_ratio * settings_.retract_before_wipe_percent;
		settings_.retract_after_wipe_percent = reduction_ratio * settings_.retract_after_wipe_percent;
	}
	
	// Calculate the pre and post wipe retraction lengths
	pre_wipe_retract_length_ = settings_.retraction_length * settings_.retract_before_wipe_percent;
	post_wipe_retract_length_ = settings_.retraction_length * settings_.retract_after_wipe_percent;
	// calculate the wipe retraction amount
	const double wipe_retraction_length = settings_.retraction_length - pre_wipe_retract_length_ - post_wipe_retract_length_;
	// calculate the wipe distance, scaled with the wipe to retraction speed ratio.
	const double wipe_retraction_speed_ratio = (settings_.wipe_feedrate / settings_.retraction_feedrate);
	const double wipe_distance = wipe_retraction_length * wipe_retraction_speed_ratio;
	// calculate the wipe distance to extrusion ratio
	distance_to_retraction_ratio_ = (settings_.retraction_length - pre_wipe_retract_length_ - post_wipe_retract_length_) / wipe_distance;
	wipe_distance_ = wipe_distance;
	half_wipe_distance_ = wipe_distance * 0.5;
	is_initialized_ = true;
}

gcode_wiper::~gcode_wiper()
{
	// p_starting_position and p_previous_starting_position can be the same item!
	if(p_starting_position_ == p_previous_starting_position_ && p_starting_position_ != NULL)
	{
		delete p_starting_position_;
		p_starting_position_ = NULL;
		p_previous_starting_position_ = NULL;
	}
	else
	{
		if (p_starting_position_ != NULL)
		{
			delete p_starting_position_;
			p_starting_position_ = NULL;
		}
		if (p_previous_starting_position_ != NULL)
		{
			delete p_previous_starting_position_;
			p_previous_starting_position_ = NULL;
		}
	}

}

void gcode_wiper::undo()
{
	restore_undo_data();
	history_.undo();
}

void gcode_wiper::save_undo_data()
{
	
	if (p_previous_starting_position_ != NULL && p_previous_starting_position_ != p_starting_position_)
	{
		delete p_previous_starting_position_;
	}

	p_previous_starting_position_ = p_starting_position_;
	previous_total_distance_ = total_distance_;
}

void gcode_wiper::restore_undo_data()
{
	if (p_starting_position_ != NULL && p_starting_position_ != p_previous_starting_position_)
	{
		delete p_starting_position_;
	}
	p_starting_position_ = p_previous_starting_position_;
	p_previous_starting_position_ = NULL;
	total_distance_ = previous_total_distance_;
	previous_total_distance_ = false;
}

void gcode_wiper::update(position& current_position, position& previous_position)
{
	if (!is_initialized_)
	{
		// Todo:  Maybe log something or throw an error?
		return;
	}
	// determine if we should clear the wipe queue.We want to clear it out in the following cases :
	// 1.  if the layer has changed
	// 2.  If the current code is not extruding
	// This is a pretty restrictive list, but allowing more wiping gets complicated.
	// Improve this if possible
	save_undo_data();
	if (
		current_position.is_layer_change_ || 
		!(current_position.has_xy_position_changed_ && current_position.is_extruding_)
	)
	{
		total_distance_ = 0;
		history_.clear();
		return;
	}
	
	if (history_.size() == 0)
	{
		if (p_starting_position_ != NULL && p_starting_position_ != p_previous_starting_position_)
			delete p_starting_position_;
		p_starting_position_ = new gcode_wiper_position(previous_position);
	}

	history_.push_back(current_position);
	previous_total_distance_ = total_distance_;
	total_distance_ += utilities::get_cartesian_distance(previous_position.x_, previous_position.y_, current_position.x_, current_position.y_);
	prune_history();
}

double gcode_wiper::get_wipe_distance()
{
	double distance;
	if (use_full_wipe_)
		distance = wipe_distance_;
	else
		distance = half_wipe_distance_;
	return distance;
}

double gcode_wiper::get_missing_retraction()
{
	if (use_full_wipe_)
	{
		return wipe_distance_to_retraction_(
			(use_full_wipe_ - total_distance_), distance_to_retraction_ratio_
		);
	}
	return wipe_distance_to_retraction_(
		(half_wipe_distance_ - total_distance_) * 2, distance_to_retraction_ratio_
	);
}

double gcode_wiper::get_extra_retraction()
{
	if(use_full_wipe_)
		return total_distance_ - wipe_distance_;
	return total_distance_ - half_wipe_distance_;
}

void gcode_wiper::prune_history()
{
	double distance = get_wipe_distance();
	// It's possible that multiple items are pruned from the beginning of the history.
	// keep track of the first pruned item.
	bool has_pruned_item = false;
	// removes items at the front of the history until removing more items would cause total_extrusion < half_retraction_length
	while (total_distance_ > distance)
	{
		// get a pointer to the first inserted history item
		gcode_wiper_position * front_item = history_.peek();

		const double distance_removed = utilities::get_cartesian_distance(
			p_starting_position_->x,
			p_starting_position_->y,
			front_item->x,
			front_item->y
		);

		const double new_total_distance = total_distance_ - distance_removed;

		if (utilities::less_than(new_total_distance, distance))
		{
			// We want to keep the total extrusion length > half_retraction_length so that
			// we can do a full wipe if possible.  Since removing this gcode would mean we can't 
			// do a full wipe, break now.
			break;
		}
		
		// Delete the starting_position if we have one, and it's not the same as the previous starting
		// position
		if (p_starting_position_ != NULL && p_previous_starting_position_ != p_starting_position_)
			delete p_starting_position_;

		// The front item is our new starting position.  Copy it.
		p_starting_position_ = new gcode_wiper_position(*front_item);
		// We've reduced the total extrusion, record the change.
		total_distance_ = new_total_distance;

		// Remove the front_item from the history list.  It will remain in the undo list
		// until after we call history_.clear() or history_.push_back()
		history_.remove();

		// Mark that we've pruned data so that we don't perform any additional
		// saves to the undo data, which will mess up our undo later.
		has_pruned_item = true;
	}
}

void gcode_wiper::clip_wipe_path(double distance_to_clip, gcode_wiper_position* &from_position, gcode_wiper_position* &to_position)
{
	
	// The gcode we want to alter will always be the EARLIEST (first) one placed in the history.
	from_position = new gcode_wiper_position(*from_position);
	to_position = new gcode_wiper_position(*to_position);
	// get the distance between the start point and the first history location
	const double distance = utilities::get_cartesian_distance(
		from_position->x, from_position->y,
		to_position->x, to_position->y
	);
	// calculate the ratio of distance to actually wipe (minus extra distance)
	// to the total distance of the line
	const double kept_distance_ratio = (distance - distance_to_clip) / distance;
	// Calculate the x and y relative change
	const double x_relative = (to_position->x - from_position->x) * kept_distance_ratio;
	const double y_relative = (to_position->y - from_position->y) * kept_distance_ratio;

	// adjust the x y values of the starting position to reflect the change in distance
	to_position->x = from_position->x + x_relative;
	to_position->y = from_position->y + y_relative;
}

void gcode_wiper::get_wipe_steps(std::vector<gcode_wiper_step*> &wipe_steps)
{

	const int num_positions = history_.size();
	if (total_distance_ == 0 || p_starting_position_ == NULL || num_positions < 1)
	{
		return;
	}
	double distance = get_wipe_distance();
	// save the starting x and y positions
	gcode_wiper_position* start_position = p_starting_position_;
	gcode_wiper_position* first_position = NULL;
	// We might have to change the start position if there is too much 
	// extrusion, but we don't want to alter the actual data.
	// If this is true later we will need to delete the start position
	bool has_altered_positions = false;
	// save the starting axis modes for convenience

	double feedrate = -1;
	
	double post_wipe_retract_length = post_wipe_retract_length_;

	// Calculate the missing retraction.  If there is any, it will be added on to the post_wipe_retraction_length
	const double missing_retraction = get_missing_retraction();

	if (utilities::greater_than_or_equal(missing_retraction, 0))
	{
		post_wipe_retract_length += missing_retraction;
	}

	// Get the current position history.
	int start_index = -1;
	std::vector<gcode_wiper_position*> positions = *history_.get_position_history(start_index);
	first_position = positions[start_index];
	
	// See if we need to alter the starting position, in the case that we have more room to wipe in the 
	// history than we need
	const double extra_distance = get_extra_retraction();

	if (utilities::greater_than(extra_distance, 0))
	{
		has_altered_positions = true;
		clip_wipe_path(extra_distance, first_position, start_position);
	}
	// Track the offset_e, which is the easiest way to calculate the gcodes later
	double current_offset_e = 0;
	double segment_distance = 0;
	double retraction_relative = 0;
	// Create a pointer to hold the previous position, which will be used as a starting point
	// for any produced gcodes
	gcode_wiper_position * previous_position = NULL;
	gcode_wiper_position * current_position = NULL;
	bool has_pre_wipe_retract_length = false;
	// do our pre-wipe retract if we need to 
	if (utilities::greater_than(pre_wipe_retract_length_, 0))
	{
		has_pre_wipe_retract_length = true;
		// We need to get the current offset e and the extruder axis mode
		// from the current extruder position, which will be the last item
		// in our history
		gcode_wiper_position * last_position = positions[positions.size() - 1];

		// Set the feedrate (it will be -1 at this time)
		// so that we know if we should alter the feedrate at all
		feedrate = settings_.retraction_feedrate;
		// Create the gcode and parameters for the final retract
		double e;
		if (last_position->is_extruder_relative)
		{
			e = -1 * pre_wipe_retract_length_;
		}
		else
		{
			e = last_position->get_offset_e() - pre_wipe_retract_length_;
		}
		wipe_steps.push_back(get_retract_step(e, feedrate));
	}

	// Set the feedrate to the wipe feedrate, the next steps will be wipe actions
	feedrate = settings_.wipe_feedrate;

	// We are going to loop through the list, first back to front, then front to back
	// However, we will only hit the starting position (p_starting_location, which was
	// added to the front of the list earlier in this function) once, and will skip its index
	// on the second pass
	for (int pass = 0; pass < 2; pass++)
	{
		int index;
		int step;
		int end;
		if (pass == 0)
		{
			index = positions.size() - 1;
			step = -1;
			end = start_index - 1;
		}
		else
		{
			index = start_index+1;
			step = 1;
			end = positions.size();
		}
		
		for (; (pass == 0 && index > end) || (pass == 1 && index < end); index = index + step)
		{
			if (index == start_index)
				current_position = first_position;
			else
				current_position = positions[index];

			if (previous_position == NULL)
			{
				previous_position = current_position;
				// use the offset E position as a convenience in case we are in absolute
				// extrusion mode.  This saves us from having to subtract the offset later.
				current_offset_e = previous_position->get_offset_e();
				if (has_pre_wipe_retract_length)
				{
					current_offset_e -= pre_wipe_retract_length_;
				}
				continue;
			}
			// calculate the distance between the previous and current position
			segment_distance = utilities::get_cartesian_distance(
				previous_position->x, previous_position->y,
				current_position->x, current_position->y
			);
			// add the extrusion length to the offset e value
			retraction_relative = wipe_distance_to_retraction_(segment_distance, distance_to_retraction_ratio_);
			gcode_wiper_step* step = get_wipe_step(previous_position, current_position, retraction_relative, current_offset_e, feedrate, pass != 0);
			wipe_steps.push_back(step);
			// the current position will be the previous position
			// for the next iteration, so save it now.
			previous_position = current_position;

			feedrate = -1;
			
		}
		if (pass == 0)
		{
			// calculate the distance between the previous and start positions
			segment_distance = utilities::get_cartesian_distance(
				previous_position->x, previous_position->y,
				start_position->x, start_position->y
			);
			retraction_relative = wipe_distance_to_retraction_(segment_distance, distance_to_retraction_ratio_);

			// Now we need to create a wipe command from the previous position to the starting point
			gcode_wiper_step* step = get_wipe_step(previous_position, start_position, retraction_relative, current_offset_e, feedrate, false);
			wipe_steps.push_back(step);
			// If we're traveling back and are at the first travel step, set the travel feedrate
			if (use_full_wipe_)
				feedrate = settings_.x_y_travel_speed;
			else
				feedrate = -1;

			// Now we need to create a wipe command from the starting point to the previous position
			step = get_wipe_step(start_position, previous_position, retraction_relative, current_offset_e, feedrate, true);
			wipe_steps.push_back(step);
		}
	}
	// It's possible we've finished wiping, but have more to 
		// retract.  If so, add the retraction now at the normal retraction
		// feedrate
	if (post_wipe_retract_length > 0 && current_position != NULL)
	{
		// Only set the feedrate if the retraction and wipe feedrates differ.
		if (!utilities::is_equal(settings_.retraction_feedrate, settings_.wipe_feedrate))
		{
			feedrate = settings_.retraction_feedrate;
		}

		// Get our e value depending on the extruder axis mode
		double e;
		if (current_position->is_extruder_relative)
		{
			e = -1 * post_wipe_retract_length;
		}
		else
		{
			e = current_offset_e - post_wipe_retract_length;
		}
		// Create the gcode and parameters for the final retract

		wipe_steps.push_back(get_retract_step(e, feedrate));
	}

	if(has_altered_positions)
	{
		delete start_position;
		start_position = NULL;
		delete first_position;
		first_position = NULL;
	}

}

gcode_wiper_step* gcode_wiper::get_wipe_step(gcode_wiper_position* start_position, gcode_wiper_position* end_position, double retraction_relative, double &current_offset_e, double feedrate, bool is_return)
{
	double x, y, e;
	if (end_position->is_relative)
	{
		x = end_position->x - start_position->x;
		y = end_position->y - start_position->y;
	}
	else
	{
		// Since we're sending gcodes, we MUST use the offset positions when using absolute
		// mode.
		x = end_position->get_offset_x();
		y = end_position->get_offset_y();
	}
	// We want to retract unless we're returning and using full wipe
	if (!(use_full_wipe_ && is_return))
	{
		
		if (end_position->is_extruder_relative)
		{
			if (is_return)
			{
				e = -1 * retraction_relative;
			}
			else
			{
				e = -1 * retraction_relative;
			}

		}
		else
		{
			if (is_return)
			{
				e = current_offset_e - retraction_relative;
				current_offset_e = e;
			}
			else
			{
				e = current_offset_e - retraction_relative;
				current_offset_e = e;
			}

		}
		return new gcode_wiper_step(x, y, e, feedrate);
	}
	else
	{
		// We are returning home, just send a travel
		return new gcode_wiper_step(x, y, feedrate);
	}
	
}

gcode_wiper_step* gcode_wiper::get_travel_step(double x, double y, double f)
{
	return new gcode_wiper_step(x, y, f);
}

gcode_wiper_step* gcode_wiper::get_retract_step(double e, double f)
{
	return new gcode_wiper_step(e, f);
}